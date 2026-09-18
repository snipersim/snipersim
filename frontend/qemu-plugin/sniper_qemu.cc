/*
 * Sniper frontend as a QEMU TCG plugin.
 *
 * Loaded into a stock QEMU with -plugin; no QEMU fork and no QEMU patches.
 * Writes SIFT, so to Sniper it is just another trace producer.
 *
 * Two modes, because QEMU offers two and they have different rules:
 *
 *   user mode    qemu-x86_64.  One guest thread per host thread, one SIFT
 *                stream each.  Flow control may block the producing thread --
 *                but only safely when the guest is single-threaded, because a
 *                vCPU blocked inside a callback still counts as running and
 *                stalls anything that needs an exclusive section (fork, code
 *                cache flush).  Multi-threaded guests are refused rather than
 *                deadlocked; see test/qemu-plugin-probe.
 *
 *   system mode  qemu-system-x86_64.  Sniper owns guest *time* through
 *                qemu_plugin_update_ns() instead of holding vCPUs, so no vCPU
 *                ever waits on another and the hazard above cannot arise.
 *
 * Instruction emission is deferred by one: SIFT wants to know whether a branch
 * was taken, which is only knowable once the next instruction's address is in
 * hand.  Each instruction is therefore buffered, its memory addresses collected
 * from the memory callbacks that follow it, and emitted when its successor
 * arrives.
 */

#include <cinttypes>
#include <cstdio>
#include <cstdlib>
#include <cstring>
#include <string>
#include <vector>
#include <algorithm>

#include <glib.h>

#include "qemu_compat.h"

extern "C" {
#include <xed-interface.h>
}

/* dl_isa, for the values RecOtherISAChange carries.  Included rather than
 * restated: the numbering is a wire format shared with the reader, and a copy
 * here would be one more place to get it wrong. */
#include "decoder.h"
#include "riscv_isa.h"
#include "ame_backend.h"

#include "sift_writer.h"
#include "sift_format.h"

extern "C" {
QEMU_PLUGIN_EXPORT int qemu_plugin_version = QEMU_PLUGIN_VERSION;
}

namespace {

/* ---------------------------------------------------------------- options */

/* The guest architecture, from QEMU's target name at install.  x86 needs the
 * XED-based branch classification and the mode probing below; RISC-V has
 * fixed-format encodings and one ISA, so its classification is a few bit
 * tests and its ISAChange record is emitted once, when the stream opens. */
enum Arch { ARCH_X86, ARCH_RISCV };
Arch        g_arch       = ARCH_X86;
uint64_t    g_nspans     = 0;        /* MemorySpans records written, all vCPUs */

std::string opt_output   = "sniper";
uint64_t    opt_flow     = 1000;   /* matches KnobFlowControl in sift/recorder */
bool        opt_verbose  = false;
bool        opt_response = false;  /* user mode: blocking flow control */
bool        opt_time     = false;  /* system mode: drive guest time */
uint64_t    opt_ns_per_insn = 1;   /* until the timing model answers */

bool in_system_mode = false;
uint64_t getcode_calls = 0, getcode_fail = 0;
const void *time_handle = nullptr;

/* --------------------------------------------------------- per-instruction */

/* Is this a *conditional* branch?
 *
 * SIFT's is_branch is narrower than "transfers control".  Both existing
 * frontends restrict it to branches with a fall-through path:
 *
 *   sift/recorder/recorder_base.cc:297  INS_IsBranch(ins) && INS_HasFallThrough(ins)
 *   frontend/dr-frontend/dr_frontend.cc:576  instr_is_cbr(instr)
 *
 * so jmp, call and ret are excluded even though they are control transfers and
 * even though decoder_lib's is_branch_opcode() lists them.  Marking them
 * aborts the simulation: an unconditional transfer decomposes into several
 * micro-ops and the one at exec_base_index is not the branch, so
 *
 *   *ERROR* Expected to find a branch here.
 *
 * The taken flag only means something for an instruction that could have gone
 * either way, which is the same set.
 */
bool xed_is_conditional_branch(const uint8_t *bytes, size_t len)
{
   xed_decoded_inst_t xedd;
   xed_decoded_inst_zero(&xedd);
   xed_decoded_inst_set_mode(&xedd, XED_MACHINE_MODE_LONG_64, XED_ADDRESS_WIDTH_64b);
   if (xed_decode(&xedd, bytes, (unsigned int)len) != XED_ERROR_NONE)
      return false;

   /* Spelled out rather than taken from XED_CATEGORY_COND_BR, which also
    * covers LOOP/LOOPE/LOOPNE -- and decoder_lib does not list those, so
    * Sniper's decoder would disagree and assert. */
   switch (xed_decoded_inst_get_iclass(&xedd))
   {
      case XED_ICLASS_JB:    case XED_ICLASS_JBE:
      case XED_ICLASS_JL:    case XED_ICLASS_JLE:
      case XED_ICLASS_JNB:   case XED_ICLASS_JNBE:
      case XED_ICLASS_JNL:   case XED_ICLASS_JNLE:
      case XED_ICLASS_JNO:   case XED_ICLASS_JNP:
      case XED_ICLASS_JNS:   case XED_ICLASS_JNZ:
      case XED_ICLASS_JO:    case XED_ICLASS_JP:
      case XED_ICLASS_JRCXZ: case XED_ICLASS_JS:
      case XED_ICLASS_JZ:
         return true;
      default:
         return false;
   }
}

/* Which x86 modes decode these bytes to exactly the length QEMU decoded them
 * to.  QEMU has already settled the true length, so a mode that disagrees is
 * not the mode this instruction ran in.
 *
 * Runs once per static instruction, at translation time, so it costs nothing at
 * execution time.  Most instructions are consistent with more than one mode --
 * `cli` is one byte in all three -- so this never identifies a mode by itself;
 * tb_mode_check() intersects the whole block's masks with what the control
 * registers permit, and that pair is what decides.
 */
enum { MODE_16 = 1, MODE_32 = 2, MODE_64 = 4 };

unsigned mode_mask_for(const uint8_t *bytes, size_t len, uint8_t qemu_len)
{
   static const struct { unsigned bit; xed_machine_mode_enum_t mm; xed_address_width_enum_t aw; }
   modes[] = {
      { MODE_16, XED_MACHINE_MODE_REAL_16,       XED_ADDRESS_WIDTH_16b },
      { MODE_32, XED_MACHINE_MODE_LONG_COMPAT_32, XED_ADDRESS_WIDTH_32b },
      { MODE_64, XED_MACHINE_MODE_LONG_64,        XED_ADDRESS_WIDTH_64b },
   };

   unsigned mask = 0;
   for (unsigned i = 0; i < sizeof(modes) / sizeof(modes[0]); i++)
   {
      xed_decoded_inst_t xedd;
      xed_state_t st = { modes[i].mm, modes[i].aw };
      xed_decoded_inst_zero_set_mode(&xedd, &st);
      if (xed_decode(&xedd, bytes, (unsigned int)len) == XED_ERROR_NONE &&
          xed_decoded_inst_get_length(&xedd) == qemu_len)
         mask |= modes[i].bit;
   }
   return mask;
}

/* Filled once per translation, shared by every execution of that instruction. */
/* RISC-V: the conditional branches are the BRANCH major opcode (0x63) and, in
 * the compressed quadrant 1, c.beqz / c.bnez (funct3 110 / 111).  Jumps
 * (jal, jalr, c.j, c.jal, c.jr, c.jalr) are excluded for the reason given
 * above for x86: SIFT's is_branch means "could have fallen through", and
 * decoder_lib's RISC-V is_branch_opcode() lists exactly the b* forms. */
bool riscv_is_conditional_branch(const uint8_t *bytes, size_t len)
{
   /* The same generated tables Sniper decodes with (riscv_isa.h): the two
      sides cannot disagree on what a branch is. */
   const unsigned length = rvisa::length_of(bytes);
   if (length == 0 || length > len)
      return false;
   return rvisa::has(rvisa::decode(rvisa::word_of(bytes, length), length), rvisa::FLAG_BRANCH);
}

struct InsnInfo
{
   uint64_t vaddr;
   uint8_t  size;
   bool     is_branch;
   unsigned mode_mask;
   /* A RISC-V vector or matrix-tile memory instruction: every access it makes
    * is recorded and sent ahead of it as MemorySpans, since two addresses
    * cannot describe a tile. */
   bool     footprint = false;
   /* RISC-V: the generated opcode id, and whether the AME backend model
    * executes the instruction (QEMU itself would trap on it). */
   unsigned op = 0;
   uint32_t word = 0;
   bool     ame = false;
};

/* ------------------------------------------------------------- per-vCPU */

struct VCPU
{
   Sift::Writer *writer   = nullptr;
   bool          closed   = false;
   unsigned      mode     = 0;   /* 0 until the first instruction settles it */
   struct qemu_plugin_register *reg_cr0  = nullptr;
   struct qemu_plugin_register *reg_efer = nullptr;
   uint64_t      icount   = 0;
   uint64_t      flow_next = 0;

   /* The instruction waiting for its successor, with the addresses its memory
    * callbacks have reported so far. */
   bool     have_pending = false;
   uint64_t pend_addr    = 0;
   uint8_t  pend_size    = 0;
   bool     pend_branch  = false;
   std::vector<uint64_t> pend_addrs;   /* the first MAX_DYNAMIC_ADDRESSES, for the Instruction record */
   /* Every access of a footprint instruction (InsnInfo::footprint). */
   struct Access { uint64_t addr; uint32_t size; bool store; };
   bool     pend_footprint = false;
   std::vector<Access> pend_accs;
   /* The AME backend this hart owns (a functional model; see ame_backend.h)
    * and the X-register handles its instructions read and write. */
   ame::Backend  ame_backend;
   struct qemu_plugin_register *xreg[32] = {nullptr};
   bool          xreg_found = false;
};

/* One slot per vCPU, sized once and never resized.
 *
 * In user mode a guest thread is a host thread and its callbacks run on it, so
 * several vCPUs call in at the same time.  Growing a shared std::vector from
 * those callbacks raced and crashed the moment a guest started a second
 * thread.  A fixed table of pointers avoids it without locking the hot path:
 * a slot is only ever written by the vCPU it belongs to, so nothing needs to
 * be shared. */
const unsigned int MAX_VCPUS = 4096;
VCPU *vcpus[MAX_VCPUS];

VCPU *vcpu_get(unsigned int idx)
{
   if (idx >= MAX_VCPUS)
   {
      fprintf(stderr, "[SNIPER-QEMU] error: vcpu %u exceeds the %u this frontend tracks\n",
              idx, MAX_VCPUS);
      exit(1);
   }
   if (!vcpus[idx])
      vcpus[idx] = new VCPU();
   return vcpus[idx];
}

/* Ask the machine what mode it is in.
 *
 * Returns the SET of decode modes the machine state permits, not a single
 * guess, because the two are genuinely different amounts of knowledge:
 *
 *   CR0.PE == 0            real mode          -> 16, and nothing else
 *   EFER.LMA == 0          protected mode     -> 32 or 16, by CS.D
 *   EFER.LMA == 1          long mode          -> 64, or 32/16 if CS.L == 0
 *
 * CS.D and CS.L live in the CS descriptor, which a plugin cannot reach: the
 * gdbstub that backs qemu_plugin_get_registers() exposes "cs" as the bare
 * selector (target/i386/gdbstub.c), and the GDT base register is not exposed
 * at all.  So the register file settles real/protected/long exactly and leaves
 * the operand-size split to the caller.
 *
 * Returns 0 when the registers are unavailable, as in user mode, where there
 * are no control registers to read.
 */
unsigned mode_from_registers(VCPU *v)
{
   if (!v->reg_cr0 || !v->reg_efer)
      return 0;

   GByteArray *buf = g_byte_array_new();
   uint64_t cr0 = 0, efer = 0;

   if (!qemu_plugin_read_register(v->reg_cr0, buf) || buf->len == 0)
   {
      g_byte_array_free(buf, TRUE);
      return 0;
   }
   memcpy(&cr0, buf->data, buf->len < sizeof(cr0) ? buf->len : sizeof(cr0));

   g_byte_array_set_size(buf, 0);
   if (!qemu_plugin_read_register(v->reg_efer, buf) || buf->len == 0)
   {
      g_byte_array_free(buf, TRUE);
      return 0;
   }
   memcpy(&efer, buf->data, buf->len < sizeof(efer) ? buf->len : sizeof(efer));
   g_byte_array_free(buf, TRUE);

   const bool protected_mode = cr0 & 1;          /* CR0.PE   */
   const bool long_mode      = (efer >> 10) & 1; /* EFER.LMA */

   if (!protected_mode)
      return MODE_16;
   if (long_mode)
      return MODE_64 | MODE_32 | MODE_16;   /* 64, or compatibility mode */
   return MODE_32 | MODE_16;                /* CS.D decides, see above */
}

/* ------------------------------------------------------------- SIFT glue */

/* Sniper asks for the bytes of code it has been told about.  In user mode the
 * guest address space is the host process's; in system mode it is not.  Either
 * way the plugin API will read it for us. */
void getCode(uint8_t *dst, const uint8_t *src, uint32_t size)
{
   /* The API fills a GByteArray rather than a caller buffer. */
   GByteArray *buf = g_byte_array_sized_new(size);
   getcode_calls++;
   if (qemu_plugin_read_memory_vaddr((uint64_t)(uintptr_t)src, buf, size) &&
       buf->len >= size)
      memcpy(dst, buf->data, size);
   else
   {
      getcode_fail++;
      memset(dst, 0, size);
   }
   g_byte_array_free(buf, TRUE);
}

void writer_open(VCPU *v, unsigned int idx)
{
   char filename[1024];
   char response[1024];

   snprintf(filename, sizeof(filename), "%s.app0.th%u.sift", opt_output.c_str(), idx);
   response[0] = '\0';
   if (opt_response)
      snprintf(response, sizeof(response), "%s_response.app0.th%u.sift",
               opt_output.c_str(), idx);

   /* Compression is off when response files are in use: the reader is reading
    * a pipe and has to see records as they are produced. */
   v->writer = new Sift::Writer(filename, getCode, !opt_response, response, idx,
                                false /* arch32 */, false /* icache per insn */,
                                false /* send va2pa */);
   if (!v->writer->IsOpen())
   {
      fprintf(stderr, "[SNIPER-QEMU] cannot open %s\n", filename);
      exit(1);
   }
   /* The reader decodes as x86 until told otherwise; a RISC-V stream says so
    * before its first instruction.  (x86 streams get theirs from the mode
    * check, which knows 16/32/64.) */
   if (g_arch == ARCH_RISCV)
      v->writer->ISAChange(dl::DL_ISA_RISCV);
   if (opt_verbose)
      fprintf(stderr, "[SNIPER-QEMU] vcpu %u -> %s\n", idx, filename);
}

/* Emit the buffered instruction, now that we know where control went next. */
/* Coalesce the pending instruction's accesses into the byte ranges Sniper
 * models: accesses of one direction that touch or overlap are merged into
 * one contiguous run, so a unit-stride vector load is one range and each
 * row of a strided tile is its own.  Two runs in one cache line still
 * cost one cache access: the micro-op model squashes the second uop of an
 * instruction that hits a line it already touched.  Runs are exact byte
 * ranges, so the recorded footprint is the bytes the instruction moved. */
void emit_spans(VCPU *v)
{
   std::vector<VCPU::Access> &a = v->pend_accs;
   std::sort(a.begin(), a.end(), [](const VCPU::Access &x, const VCPU::Access &y) {
      return x.store != y.store ? !x.store : x.addr < y.addr; });
   Sift::MemorySpan spans[Sift::MAX_MEMORY_SPANS];
   unsigned n = 0;
   bool truncated = false;
   for (const VCPU::Access &acc : a)
   {
      const uint64_t end = acc.addr + acc.size;
      if (n && (spans[n - 1].is_write == (uint8_t)acc.store)
          && acc.addr <= spans[n - 1].addr + spans[n - 1].size)
      {
         const uint64_t cur_end = spans[n - 1].addr + spans[n - 1].size;
         if (end > cur_end)
            spans[n - 1].size = (uint32_t)(end - spans[n - 1].addr);
         continue;
      }
      if (n == Sift::MAX_MEMORY_SPANS)
      {
         truncated = true;
         break;
      }
      spans[n].addr = acc.addr;
      spans[n].size = acc.size;
      spans[n].is_write = acc.store;
      n++;
   }
   static bool warned = false;
   if (truncated && !warned)
   {
      warned = true;
      fprintf(stderr, "[SNIPER-QEMU] an instruction at 0x%" PRIx64 " touched more than %u byte ranges; "
              "the rest are not modelled\n", v->pend_addr, Sift::MAX_MEMORY_SPANS);
   }
   v->writer->MemorySpans((uint8_t)n, spans);
   __sync_fetch_and_add(&g_nspans, 1);
}

void flush_pending(VCPU *v, uint64_t next_vaddr)
{
   if (!v->have_pending)
      return;

   const bool taken = v->pend_branch &&
                      (next_vaddr != v->pend_addr + v->pend_size);

   if (v->pend_footprint && !v->pend_accs.empty())
      emit_spans(v);

   v->writer->Instruction(v->pend_addr, v->pend_size,
                          (uint8_t)v->pend_addrs.size(),
                          v->pend_addrs.data(),
                          v->pend_branch, taken,
                          false /* is_predicate */, true /* executed */);

   v->have_pending = false;
   v->pend_addrs.clear();
   v->pend_accs.clear();
   v->icount++;
}

}  /* namespace */

namespace {

/* ------------------------------------------------------------ callbacks */

/* AME: executed by the backend model, since QEMU has no AME.  Registered
 * after the tracer's callback for the same instruction, with RW_REGS_PC, so
 * the trace record is already pending when this runs; the model's memory
 * accesses go into the pending footprint, then the PC is moved past the
 * instruction before QEMU's illegal-instruction code for it can run (that
 * exits the CPU loop; see test/qemu-plugin-probe/emu-probe). */
void ame_exec(unsigned int cpu_index, void *udata)
{
   const InsnInfo *ii = (const InsnInfo *)udata;
   VCPU *v = vcpu_get(cpu_index);
   if (!v->xreg_found)
   {
      /* QEMU's gdb register list names X registers by ABI name, with x8 as
       * "fp" rather than "s0"; a register without a handle would read as
       * zero, so a miss is reported loudly instead. */
      GArray *regs = qemu_plugin_get_registers();
      for (guint i = 0; i < regs->len; i++)
      {
         qemu_plugin_reg_descriptor *d = &g_array_index(regs, qemu_plugin_reg_descriptor, i);
         for (unsigned r = 0; r < 32; r++)
         {
            char xn[8]; snprintf(xn, sizeof(xn), "x%u", r);
            if (strcmp(d->name, rvisa::xreg_name(r)) == 0 || strcmp(d->name, xn) == 0 || (r == 8 && strcmp(d->name, "fp") == 0))
               v->xreg[r] = d->handle;
         }
      }
      g_array_free(regs, TRUE);
      for (unsigned r = 1; r < 32; r++)
         if (!v->xreg[r])
            fprintf(stderr, "[AME] no register handle for %s: its reads return 0 and writes are dropped\n", rvisa::xreg_name(r));
      v->xreg_found = true;
   }
   ame::Env env;
   env.read_x = [v](unsigned r) -> uint64_t {
      if (r == 0 || !v->xreg[r]) return 0;
      GByteArray *b = g_byte_array_new(); uint64_t val = 0;
      if (qemu_plugin_read_register(v->xreg[r], b) && b->len >= 8) memcpy(&val, b->data, 8);
      g_byte_array_free(b, TRUE); return val;
   };
   env.write_x = [v](unsigned r, uint64_t val) {
      if (r == 0 || !v->xreg[r]) return;
      GByteArray *b = g_byte_array_new(); g_byte_array_append(b, (const guint8 *)&val, 8);
      qemu_plugin_write_register(v->xreg[r], b); g_byte_array_free(b, TRUE);
   };
   env.read_mem = [](uint64_t addr, void *dst, unsigned n) -> bool {
      GByteArray *b = g_byte_array_new(); bool ok = qemu_plugin_read_memory_vaddr(addr, b, n) && b->len == n;
      if (ok) memcpy(dst, b->data, n);
      g_byte_array_free(b, TRUE); return ok;
   };
   env.write_mem = [](uint64_t addr, const void *src, unsigned n) -> bool {
      GByteArray *b = g_byte_array_new(); g_byte_array_append(b, (const guint8 *)src, n);
      bool ok = qemu_plugin_write_memory_vaddr(addr, b); g_byte_array_free(b, TRUE); return ok;
   };
   env.note_access = [v](uint64_t addr, unsigned n, bool store) {
      if (!v->have_pending) return;
      if (v->pend_addrs.size() < Sift::MAX_DYNAMIC_ADDRESSES) v->pend_addrs.push_back(addr);
      VCPU::Access acc; acc.addr = addr; acc.size = n; acc.store = store;
      v->pend_accs.push_back(acc);
   };
   env.error = [ii](const std::string &why) {
      fprintf(stderr, "[AME] error at 0x%" PRIx64 ": %s\n", ii->vaddr, why.c_str());
   };
   ame::execute(v->ame_backend, ii->op, rvisa::word_of((const uint8_t *)&ii->word, 4), env);
   qemu_plugin_set_pc(ii->vaddr + ii->size);
}

void insn_exec(unsigned int cpu_index, void *udata)
{
   const InsnInfo *ii = (const InsnInfo *)udata;
   VCPU *v = vcpu_get(cpu_index);

   /* The previous instruction's fate is now known. */
   flush_pending(v, ii->vaddr);


   v->pend_addr   = ii->vaddr;
   v->pend_size   = ii->size;
   v->pend_branch = ii->is_branch;
   v->pend_footprint = ii->footprint;
   v->have_pending = true;

   /* Pacing.  In system mode this hands Sniper's notion of time to QEMU and
    * never blocks.  In user mode it is the blocking Sync(), which is why
    * multi-threaded guests are refused at startup. */
   if (v->icount >= v->flow_next)
   {
      v->flow_next = v->icount + opt_flow;

      if (opt_response)
      {
         v->writer->Sync();            /* blocks until Sniper answers */
      }
      else if (opt_time && time_handle)
      {
         static uint64_t virt_ns;      /* time only moves forward */
         uint64_t want = v->icount * opt_ns_per_insn;
         if (want > virt_ns)
         {
            virt_ns = want;
            qemu_plugin_update_ns(time_handle, (int64_t)want);
         }
      }
   }
}

/* Decide the decode mode for one translation block.
 *
 * Once per block is the exact granularity, not an approximation of one.  QEMU
 * keys every TB on env->hflags (target/i386/tcg/tcg-cpu.c,
 * x86_get_tb_cpu_state), and hflags carries HF_PE/HF_CS32/HF_CS64 -- so a
 * block that changed mode partway through would be a block whose lookup key
 * changed partway through, which cannot happen.  QEMU's own decoder relies on
 * this: CODE64(s) and CODE32(s) are read straight out of the block's flags.  A
 * mode switch therefore always ends a block, and checking here cannot miss one
 * or place one in the wrong instruction.
 *
 * Registered QEMU_PLUGIN_CB_R_REGS so the control registers are live, and it
 * runs before the block's first instruction callback, so the ISAChange record
 * precedes the instructions it governs.
 */
struct TBInfo
{
   uint64_t vaddr    = 0;   /* first instruction, for diagnostics */
   unsigned mode_mask = 0;  /* modes every instruction here is consistent with */
};

void tb_mode_check(unsigned int cpu_index, void *udata)
{
   const TBInfo *ti = (const TBInfo *)udata;
   const unsigned encoded = ti->mode_mask;
   VCPU *v = vcpu_get(cpu_index);

   /* Two independent sources, consulted every block.
    *
    * The architectural one is authoritative but incomplete: it cannot see CS.D,
    * so it cannot split 32 from 16.  The encoding one is complete but not
    * authoritative: it reports every mode in which all of this block's bytes
    * decode to the lengths QEMU used, which is usually more than one mode
    * because most encodings are legal in several.
    *
    * Intersecting them is what makes this exact wherever exactness is
    * available -- the registers rule out modes the encodings allow, and the
    * encodings rule out modes the registers allow. */
   const unsigned arch = mode_from_registers(v);
   unsigned cand = arch & encoded;

   /* Disagreement means one side is blind, not that the state is impossible:
    * user mode has no control registers (arch == 0), and a block XED could not
    * decode has no encoding evidence (encoded == 0).  Prefer the architectural
    * answer, which is never wrong when it is available at all. */
   if (!cand)
      cand = arch ? arch : encoded;
   if (!cand)
      return;                      /* neither source said anything */

   /* Mode changes are rare, so hold the current mode whenever it is still
    * consistent.  This is what keeps an ambiguous block -- one whose bytes
    * decode identically in 16 and 32, say -- from flipping the mode back and
    * forth and filling the trace with records that describe nothing. */
   if (v->mode & cand)
      return;

   /* Forced to move.  Where the candidates still leave a choice it is always
    * the operand-size split the registers could not see, and the wider one is
    * right in every case that reaches here: protected mode is 32-bit unless a
    * 16-bit code segment says otherwise, long mode is 64-bit unless a
    * compatibility segment says otherwise, and in both cases "otherwise" would
    * have shown up as a narrower encoding mask. */
   const unsigned m = (cand & MODE_64) ? MODE_64
                    : (cand & MODE_32) ? MODE_32 : MODE_16;

   v->mode = m;
   if (v->writer)
      v->writer->ISAChange(m == MODE_64 ? dl::DL_ISA_X86_64
                         : m == MODE_32 ? dl::DL_ISA_IA32 : dl::DL_ISA_IA16);
   if (opt_verbose)
      fprintf(stderr, "[SNIPER-QEMU] vcpu %u now in %s mode at 0x%" PRIx64 "\n",
              cpu_index, m == MODE_64 ? "64-bit" : m == MODE_32 ? "32-bit" : "16-bit",
              ti->vaddr);
}

void mem_access(unsigned int cpu_index, qemu_plugin_meminfo_t info,
                uint64_t vaddr, void *udata)
{
   VCPU *v = vcpu_get(cpu_index);
   if (!v->have_pending)
      return;
   if (v->pend_addrs.size() < Sift::MAX_DYNAMIC_ADDRESSES)
      v->pend_addrs.push_back(vaddr);
   if (v->pend_footprint)
   {
      VCPU::Access acc;
      acc.addr  = vaddr;
      acc.size  = 1u << qemu_plugin_mem_size_shift(info);
      acc.store = qemu_plugin_mem_is_store(info);
      v->pend_accs.push_back(acc);
   }
}

void syscall_enter(SNP_SYSCALL_ARGS)
{
   VCPU *v = vcpu_get(vcpu_index);
   if (!v->writer)
      return;

   /* Emitting a syscall record asks Sniper a question, so it is only valid
    * when there is a response channel to answer on -- Sift::Writer::Syscall()
    * calls initResponse() unconditionally.  The Pin recorder gates the same
    * thing on KnobEmulateSyscalls.  Without a channel we are writing a trace
    * to a file and the syscall is simply not modelled. */
   if (!opt_response)
      return;

   /* Close out the instruction that issued the syscall first. */
   flush_pending(v, 0);

   const uint64_t args[6] = { a1, a2, a3, a4, a5, a6 };
   v->writer->Syscall((uint16_t)num, (const char *)args, sizeof(args));
}

void vcpu_init(SNP_VCPU_ARGS)
{
   VCPU *v = vcpu_get(cpu_index);

   /* Checked before anything is opened, so a user who asks for this gets the
    * explanation rather than whatever the SIFT writer says about its response
    * channel.
    *
    * A second guest thread in user mode is the configuration that deadlocks:
    * a vCPU blocked in Sync() still counts as running, so anything needing an
    * exclusive section waits for it.  Refuse it rather than hang. */
   if (!in_system_mode && opt_response && cpu_index > 0)
   {
      fprintf(stderr,
              "[SNIPER-QEMU] error: the guest started a second thread.\n"
              "  User-mode flow control can only be used with single-threaded\n"
              "  guests -- a vCPU blocked in a plugin callback stalls any other\n"
              "  vCPU that needs an exclusive section, and deadlocks when the\n"
              "  timing model holds it.  Use full-system mode for multi-core,\n"
              "  or the Pin/DynamoRIO frontends.\n");
      exit(1);
   }

   if (!v->writer)
      writer_open(v, cpu_index);

   /* Register handles are per-vCPU and only obtainable in vCPU context.  Absent
    * in user mode, where the mode never changes anyway. */
   GArray *regs = qemu_plugin_get_registers();
   for (guint i = 0; regs && i < regs->len; i++)
   {
      qemu_plugin_reg_descriptor *d =
         &g_array_index(regs, qemu_plugin_reg_descriptor, i);
      if (!strcmp(d->name, "cr0"))
         v->reg_cr0 = d->handle;
      else if (!strcmp(d->name, "efer"))
         v->reg_efer = d->handle;
   }
   if (regs)
      g_array_free(regs, TRUE);
}

/* Close a vCPU's stream exactly once.  End() must be the last record: writing
 * anything after it produces a trace that siftdump crashes on and that Sniper
 * waits forever for the end of. */
void writer_close(VCPU *v)
{
   if (!v->writer || v->closed)
      return;
   flush_pending(v, 0);
   v->writer->End();
   v->closed = true;
}

void vcpu_exit(SNP_VCPU_ARGS)
{
   writer_close(vcpu_get(cpu_index));
}

void tb_trans(SNP_TB_TRANS_ARGS)
{
   const size_t n = qemu_plugin_tb_n_insns(tb);
   TBInfo *ti = new TBInfo();
   ti->mode_mask = MODE_16 | MODE_32 | MODE_64;

   for (size_t i = 0; i < n; i++)
   {
      struct qemu_plugin_insn *insn = qemu_plugin_tb_get_insn(tb, i);

      /* One record per static instruction, reused by every execution. */
      InsnInfo *ii = new InsnInfo();
      ii->vaddr = qemu_plugin_insn_vaddr(insn);
      if (i == 0)
         ti->vaddr = ii->vaddr;
      ii->size  = (uint8_t)qemu_plugin_insn_size(insn);

      /* Decoded once per translation, so the cost is amortised over every
       * execution of the block. */
      uint8_t bytes[XED_MAX_INSTRUCTION_BYTES] = {0};
      size_t got = qemu_plugin_insn_data(insn, bytes, sizeof(bytes));
      if (g_arch == ARCH_RISCV)
      {
         ii->is_branch = got ? riscv_is_conditional_branch(bytes, got) : false;
         ii->mode_mask = 0;
         {
            const unsigned length = got ? rvisa::length_of(bytes) : 0;
            if (length && length <= got)
            {
               ii->word = rvisa::word_of(bytes, length);
               ii->op = rvisa::decode(ii->word, length);
               ii->footprint = rvisa::has(ii->op, rvisa::FLAG_FOOTPRINT);
               const char *cls = rvisa::info(rvisa::class_of(ii->op)).name;
               ii->ame = ii->op && strncmp(cls, "ame_", 4) == 0;
            }
         }
      }
      else
      {
         ii->is_branch = got ? xed_is_conditional_branch(bytes, got) : false;
         ii->mode_mask = got ? mode_mask_for(bytes, got, ii->size) : 0;
      }
      if (ii->mode_mask)
         ti->mode_mask &= ii->mode_mask;  /* one mode governs the whole block */

      qemu_plugin_register_vcpu_insn_exec_cb(insn, insn_exec,
                                             QEMU_PLUGIN_CB_NO_REGS, ii);
      if (ii->ame)
         qemu_plugin_register_vcpu_insn_exec_cb(insn, ame_exec, QEMU_PLUGIN_CB_RW_REGS_PC, ii);
      qemu_plugin_register_vcpu_mem_cb(insn, mem_access,
                                       QEMU_PLUGIN_CB_NO_REGS,
                                       QEMU_PLUGIN_MEM_RW, nullptr);
   }

   /* R_REGS so mode_from_registers() has something to read; it runs before the
    * block's first instruction callback, so an ISAChange precedes the
    * instructions it applies to. */
   if (g_arch == ARCH_X86)
      qemu_plugin_register_vcpu_tb_exec_cb(tb, tb_mode_check,
                                           QEMU_PLUGIN_CB_R_REGS, ti);
   else
      delete ti;                    /* one ISA, nothing to check per block */
}

void at_exit(SNP_UDATA_ARGS)
{
   uint64_t total = 0;
   for (size_t i = 0; i < MAX_VCPUS; i++)
   {
      VCPU *v = vcpus[i];
      if (!v || !v->writer)
         continue;
      writer_close(v);
      delete v->writer;
      v->writer = nullptr;
      total += v->icount;
      if (opt_verbose)
         fprintf(stderr, "[SNIPER-QEMU] vcpu %zu: %" PRIu64 " instructions\n", i, v->icount);
   }
   fprintf(stderr, "[SNIPER-QEMU] %" PRIu64 " instructions written\n", total);
   if (g_nspans)
      fprintf(stderr, "[SNIPER-QEMU] %" PRIu64 " memory footprints recorded\n", g_nspans);
   if (opt_verbose || getcode_fail)
      fprintf(stderr, "[SNIPER-QEMU] guest code reads: %" PRIu64 ", failed: %" PRIu64 "\n",
              getcode_calls, getcode_fail);
}

}  /* namespace */

extern "C"
QEMU_PLUGIN_EXPORT int qemu_plugin_install(qemu_plugin_id_t id,
                                           const qemu_info_t *info,
                                           int argc, char **argv)
{
   in_system_mode = info->system_emulation;
   if (!strncmp(info->target_name, "riscv", 5))
      g_arch = ARCH_RISCV;
   else if (!strcmp(info->target_name, "x86_64") || !strcmp(info->target_name, "i386"))
      g_arch = ARCH_X86;
   else
   {
      fprintf(stderr, "[SNIPER-QEMU] unsupported target %s (x86_64, i386, riscv32, riscv64)\n",
              info->target_name);
      return -1;
   }

   /* Needed before any xed_decode(). */
   xed_tables_init();

   for (int i = 0; i < argc; i++)
   {
      const char *eq = strchr(argv[i], '=');
      const std::string key = eq ? std::string(argv[i], eq - argv[i]) : std::string(argv[i]);
      const std::string val = eq ? std::string(eq + 1) : std::string();

      if (key == "output")        opt_output = val;
      else if (key == "flow")     opt_flow = strtoull(val.c_str(), nullptr, 0);
      else if (key == "verbose")  opt_verbose = true;
      else if (key == "response") opt_response = true;
      else if (key == "time")     opt_time = true;
      else if (key == "ns")       opt_ns_per_insn = strtoull(val.c_str(), nullptr, 0);
      else
      {
         fprintf(stderr, "[SNIPER-QEMU] unknown option '%s'\n", argv[i]);
         return -1;
      }
   }

   if (opt_response && in_system_mode)
   {
      fprintf(stderr, "[SNIPER-QEMU] error: response-file flow control is user mode only.\n"
                      "  In full system, Sniper paces the guest with 'time' instead.\n");
      return -1;
    }

   if (opt_time)
   {
      time_handle = qemu_plugin_request_time_control();
      if (!time_handle)
      {
         fprintf(stderr, "[SNIPER-QEMU] error: another plugin already controls time.\n");
         return -1;
      }
   }

   SNP_REG_TB_TRANS(id, tb_trans);
   SNP_REG_SYSCALL(id, syscall_enter);
   SNP_REG_VCPU_INIT(id, vcpu_init);
   SNP_REG_VCPU_EXIT(id, vcpu_exit);
   SNP_REG_ATEXIT(id, at_exit);
   return 0;
}
