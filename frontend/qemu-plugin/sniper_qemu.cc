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
#include "acc_backend.h"
#include "qemu_plugin_ext.h"
#include <atomic>
#include <mutex>
#include <unordered_set>

#include "sift_writer.h"
#include "sim_api.h"         /* the SIM_CMD_ numbers */
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
enum Arch { ARCH_X86, ARCH_RISCV, ARCH_AARCH64 };
Arch        g_arch       = ARCH_X86;
uint64_t    g_nspans     = 0;        /* MemorySpans records written, all vCPUs */
uint64_t    g_ndropped_lines = 0;  /* cache lines an instruction touched beyond the spans its record holds */
/* Instructions the plugin's models execute (AME, Xsnacc): claimed at
 * translation when the QEMU has the claim API (the block continues through
 * them), otherwise emulated from the illegal-instruction path with a loop
 * exit per instruction.  opt_claim=0 forces the older path for comparison. */
bool        opt_claim = true;
/* Code rewritten in place (Sift::CodeTracker): QEMU re-translates code after
 * it is written, and tb_trans reports the new bytes.  smc=0 turns it off. */
bool        opt_smc = true;
bool        opt_trace = true;      /* trace=0: no SIFT output and no tracing callbacks, only the plugin models (a functional run) */
uint64_t    g_exec_claimed = 0, g_exec_trapped = 0;
std::mutex  g_claim_lock;
std::unordered_set<const struct qemu_plugin_insn *> g_claimed;   /* claimed in this translation, consumed by tb_trans */

std::string opt_output   = "sniper";
uint64_t    opt_flow     = 1000;   /* matches KnobFlowControl in sift/recorder */
bool        opt_verbose  = false;
bool        opt_response = false;  /* user mode: blocking flow control */
/* roi: trace only between SimRoiStart() and SimRoiEnd(), as the SDE and
 * DynamoRIO front ends do with -roi.  The magic instructions themselves are
 * always sent, so the simulator sees the markers either way. */
bool        opt_roi      = false;
std::atomic<bool> g_tracing{true};
unsigned    opt_app      = 0;      /* the app number in the file names (record-trace -s) */
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

/* AArch64: SIFT's is_branch is "could have fallen through", as for x86 and
 * RISC-V above: B.cond (and BC.cond), CBZ/CBNZ and TBZ/TBNZ, both widths --
 * the set decoder_lib's ARMDecodedInst::is_conditional_branch() and the
 * DynamoRIO front end's instr_is_cbr() agree on.  Fixed 4-byte encodings, so
 * no decoder is needed.  B.cond with AL or NV is architecturally
 * unconditional, and the decoder says so too. */
bool aarch64_is_conditional_branch(const uint8_t *bytes, size_t len)
{
   if (len < 4)
      return false;
   const uint32_t w = (uint32_t)bytes[0] | ((uint32_t)bytes[1] << 8) | ((uint32_t)bytes[2] << 16) | ((uint32_t)bytes[3] << 24);
   if ((w & 0xff000000u) == 0x54000000u)          /* B.cond / BC.cond */
      return (w & 0xeu) != 0xeu;                    /* cond 1110 (AL), 1111 (NV) */
   if ((w & 0x7e000000u) == 0x34000000u)          /* CBZ / CBNZ */
      return true;
   if ((w & 0x7e000000u) == 0x36000000u)          /* TBZ / TBNZ */
      return true;
   return false;
}

/* The magic instruction of sim_api.h, by architecture:
 *
 *   x86      xchg %bx, %bx            66 87 db
 *   AArch64  bfm x0, x0, #0, #0       b3400000
 *   RISC-V   slti zero, zero, 0x5a    05a02013
 *
 * Each is a no-op on hardware, which is what lets a program carry them. */
bool is_magic(const uint8_t *bytes, size_t len);

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

bool is_magic(const uint8_t *bytes, size_t len)
{
   static const uint8_t x86[]   = { 0x66, 0x87, 0xdb };
   static const uint8_t a64[]   = { 0x00, 0x00, 0x40, 0xb3 };
   static const uint8_t riscv[] = { 0x13, 0x20, 0xa0, 0x05 };
   const uint8_t *want = g_arch == ARCH_X86 ? x86 : g_arch == ARCH_AARCH64 ? a64 : riscv;
   const size_t n      = g_arch == ARCH_X86 ? sizeof(x86) : 4;
   return len >= n && memcmp(bytes, want, n) == 0;
}

struct InsnInfo
{
   uint64_t vaddr;
   /* System mode: the physical address of the instruction, and of the page
    * after if it crosses into one (0: not known).  Fixed per translation:
    * QEMU finds a translated block by its physical page, so the block that
    * runs is always this code at this address. */
   uint64_t paddr  = 0;
   uint64_t paddr2 = 0;
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
   bool     claimed = false;    /* claimed at translation: no exception follows, so no set_pc */
   bool     magic = false;      /* sim_api.h's magic instruction (is_magic) */
};

/* ------------------------------------------------------------- per-vCPU */

/* A data access to a device (MMIO): the page maps no memory, and a mapping
 * sent for it earlier -- the guest reused the virtual range -- is withdrawn */
const uint64_t PADDR_IO = ~0ULL;

struct VCPU
{
   Sift::Writer *writer   = nullptr;
   bool          closed   = false;
   bool          pregs    = false;   /* the AME register set is registered with the gdbstub */
   bool          pregs_checked = false;
   unsigned      mode     = 0;   /* 0 until the first instruction settles it */
   struct qemu_plugin_register *reg_cr0  = nullptr;
   struct qemu_plugin_register *reg_efer = nullptr;
   uint64_t      icount   = 0;
   uint64_t      flow_next = 0;

   /* The instruction waiting for its successor, with the addresses its memory
    * callbacks have reported so far. */
   bool     have_pending = false;
   uint64_t pend_addr    = 0;
   uint64_t pend_paddr   = 0;
   uint64_t pend_paddr2  = 0;
   uint8_t  pend_size    = 0;
   bool     pend_branch  = false;
   std::vector<uint64_t> pend_addrs;   /* the first MAX_DYNAMIC_ADDRESSES, for the Instruction record */
   std::vector<uint64_t> pend_paddrs;  /* system mode: their physical addresses (0: not known; PADDR_IO: a device) */
   /* System mode: the pages after the first of accesses that cross a page,
    * with their physical addresses, which the simulator cannot derive from
    * the first page's */
   std::vector<std::pair<uint64_t, uint64_t> > pend_more_pages;
   /* Every access of a footprint instruction (InsnInfo::footprint). */
   struct Access { uint64_t addr; uint32_t size; bool store; uint64_t paddr = 0; };
   bool     pend_footprint = false;
   std::vector<Access> pend_accs;
   /* The AME backend this hart owns (a functional model; see ame_backend.h)
    * and the X-register handles its instructions read and write. */
   ame::Backend  ame_backend;
   acc::Backend  acc_backend;      /* the DMA-attached accelerator's functional model (Xsnacc) */
   struct qemu_plugin_register *xreg[32] = {nullptr};
   bool          xreg_found = false;
   /* The magic instruction's registers: the answer, the command and its two
    * arguments (x86 rax rax rbx rcx; AArch64 x0 x1 x2 x3; RISC-V a0 a0 a1 a2). */
   struct qemu_plugin_register *mreg[4] = {nullptr};
   bool          mreg_have[4] = {false};   /* a null handle is valid: register 0 (rax, x0) */
   bool          mreg_found = false;
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

/* The simulator reading or writing guest memory while it answers a question
 * -- a magic instruction's string argument (SimSetThreadName,
 * SimNamedMarker), an emulated system call's buffers.  Without this the
 * writer had no way to answer and the simulator waited forever: a guest that
 * named a thread before its ROI hung run-sniper --frontend=qemu.  Called in
 * the vCPU's own callback, where QEMU's memory API works on its addresses. */
bool access_memory(void *, Sift::MemoryLockType, Sift::MemoryOpType op,
                   uint64_t addr, uint8_t *data, uint32_t size)
{
   GByteArray *b = g_byte_array_new();
   bool ok;
   if (op == Sift::MemRead)
   {
      ok = qemu_plugin_read_memory_vaddr(addr, b, size) && b->len == size;
      if (ok)
         memcpy(data, b->data, size);
      else
         memset(data, 0, size);
   }
   else
   {
      g_byte_array_append(b, data, size);
      ok = qemu_plugin_write_memory_vaddr(addr, b);
   }
   g_byte_array_free(b, TRUE);
   if (!ok)
      fprintf(stderr, "[SNIPER-QEMU] the simulator %s %u bytes at 0x%" PRIx64 ", which the guest cannot reach\n",
              op == Sift::MemRead ? "read" : "wrote", size, addr);
   return ok;
}

void writer_open(VCPU *v, unsigned int idx)
{
   char filename[1024];
   char response[1024];

   snprintf(filename, sizeof(filename), "%s.app%u.th%u.sift", opt_output.c_str(), opt_app, idx);
   response[0] = '\0';
   if (opt_response)
      snprintf(response, sizeof(response), "%s_response.app%u.th%u.sift",
               opt_output.c_str(), opt_app, idx);

   /* Compression is off when response files are in use: the reader is reading
    * a pipe and has to see records as they are produced. */
   v->writer = new Sift::Writer(filename, getCode, !opt_response, response, idx,
                                false /* arch32 */, false /* icache per insn */,
                                false /* send va2pa */, NULL, NULL,
                                in_system_mode /* code and data by physical address */);
   if (!v->writer->IsOpen())
   {
      fprintf(stderr, "[SNIPER-QEMU] cannot open %s\n", filename);
      exit(1);
   }
   v->writer->setHandleAccessMemoryFunc(access_memory);
   /* The reader decodes as x86 until told otherwise; a RISC-V stream says so
    * before its first instruction.  (x86 streams get theirs from the mode
    * check, which knows 16/32/64.) */
   if (g_arch == ARCH_RISCV)
      v->writer->ISAChange(dl::DL_ISA_RISCV);
   else if (g_arch == ARCH_AARCH64)
      v->writer->ISAChange(dl::DL_ISA_AARCH64);
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
   /* Contiguous (or overlapping) runs of the same direction become one span. */
   std::vector<Sift::MemorySpan> runs;
   for (const VCPU::Access &acc : a)
   {
      const uint64_t end = acc.addr + acc.size;
      if (!runs.empty() && (runs.back().is_write == (uint8_t)acc.store)
          && acc.addr <= runs.back().addr + runs.back().size)
      {
         const uint64_t cur_end = runs.back().addr + runs.back().size;
         if (end > cur_end)
            runs.back().size = (uint32_t)(end - runs.back().addr);
         continue;
      }
      Sift::MemorySpan sp; sp.addr = acc.addr; sp.size = acc.size; sp.is_write = acc.store;
      runs.push_back(sp);
   }
   /* More runs than the record holds (a gather over thousands of lines at a
      wide VLEN): coalesce by cache line -- what the core would fetch -- keep
      the first MAX_MEMORY_SPANS lines, and count the lines beyond them so
      the trace still says how much the instruction touched. */
   uint32_t dropped = 0;
   if (runs.size() > Sift::MAX_MEMORY_SPANS)
   {
      std::vector<Sift::MemorySpan> lines;
      for (const Sift::MemorySpan &r : runs)
      {
         const uint64_t first = r.addr & ~63ull, last = (r.addr + r.size - 1) & ~63ull;
         for (uint64_t l = first; l <= last; l += 64)
         {
            if (!lines.empty() && lines.back().is_write == r.is_write && l >= lines.back().addr && l < lines.back().addr + lines.back().size)
               continue;                      /* the same line again */
            if (!lines.empty() && lines.back().is_write == r.is_write && lines.back().addr + lines.back().size == l)
               lines.back().size += 64;
            else
            {
               Sift::MemorySpan sp; sp.addr = l; sp.size = 64; sp.is_write = r.is_write;
               lines.push_back(sp);
            }
         }
      }
      if (lines.size() > Sift::MAX_MEMORY_SPANS)
      {
         for (size_t i = Sift::MAX_MEMORY_SPANS; i < lines.size(); i++)
            dropped += lines[i].size / 64;
         lines.resize(Sift::MAX_MEMORY_SPANS);
      }
      runs.swap(lines);
   }
   Sift::MemorySpan spans[Sift::MAX_MEMORY_SPANS];
   const unsigned n = (unsigned)runs.size();
   for (unsigned i = 0; i < n; i++) spans[i] = runs[i];
   if (dropped) __sync_fetch_and_add(&g_ndropped_lines, dropped);
   v->writer->MemorySpans((uint8_t)n, spans, dropped);
   __sync_fetch_and_add(&g_nspans, 1);
}

void flush_pending(VCPU *v, uint64_t next_vaddr)
{
   if (!v->have_pending)
      return;

   const bool taken = v->pend_branch &&
                      (next_vaddr != v->pend_addr + v->pend_size);

   /* System mode: where this instruction's code and data are in physical
    * memory, which the writer passes on when it changed -- a context switch
    * puts another process's pages at the same virtual addresses.  The
    * simulator then caches by physical address, and code is told apart by
    * its physical page. */
   if (in_system_mode)
   {
      if (v->pend_paddr)
         v->writer->Mapping(v->pend_addr, v->pend_paddr);
      if (v->pend_paddr2)
         v->writer->Mapping((v->pend_addr + v->pend_size - 1) & ~0xfffULL, v->pend_paddr2);
      for (size_t i = 0; i < v->pend_addrs.size() && i < v->pend_paddrs.size(); i++)
         if (v->pend_paddrs[i] == PADDR_IO)
            v->writer->Mapping(v->pend_addrs[i], Sift::Writer::NO_PHYSICAL_PAGE);
         else if (v->pend_paddrs[i])
            v->writer->Mapping(v->pend_addrs[i], v->pend_paddrs[i]);
      for (const std::pair<uint64_t, uint64_t> &m : v->pend_more_pages)
         v->writer->Mapping(m.first, m.second);
      for (const VCPU::Access &a : v->pend_accs)
         if (a.paddr)
            v->writer->Mapping(a.addr, a.paddr);
   }

   if (v->pend_footprint && !v->pend_accs.empty())
      emit_spans(v);

   v->writer->Instruction(v->pend_addr, v->pend_size,
                          (uint8_t)v->pend_addrs.size(),
                          v->pend_addrs.data(),
                          v->pend_branch, taken,
                          false /* is_predicate */, true /* executed */);

   v->have_pending = false;
   v->pend_addrs.clear();
   v->pend_paddrs.clear();
   v->pend_more_pages.clear();
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
   if (v->pregs && !v->pregs_checked)
   {
      /* The unit's registers went into the gdbstub list at vcpu init; read
       * one back the way gdb and other plugins would, and say so. */
      v->pregs_checked = true;
      GArray *regs = qemu_plugin_get_registers();
      unsigned seen = 0; struct qemu_plugin_register *nlen = nullptr;
      for (guint i = 0; i < regs->len; i++)
      {
         qemu_plugin_reg_descriptor *d = &g_array_index(regs, qemu_plugin_reg_descriptor, i);
         if (d->feature && strcmp(d->feature, "org.riscv.ame") == 0) seen++;
         if (strcmp(d->name, "amenlen") == 0) nlen = d->handle;
      }
      g_array_free(regs, TRUE);
      uint64_t val = 0;
      if (nlen) { GByteArray *b = g_byte_array_new(); if (qemu_plugin_read_register(nlen, b) && b->len >= 8) memcpy(&val, b->data, 8); g_byte_array_free(b, TRUE); }
      fprintf(stderr, "[SNIPER-QEMU] AME registers: %u in the gdbstub list under org.riscv.ame, amenlen reads %" PRIu64 "\n", seen, val);
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
   if (SNP_HAVE_RAISE)
      env.trap = [ii, v](unsigned cause, uint64_t tval) {
         /* the pending trace record of this instruction is dropped with the
          * guest's state: nothing retires */
         v->have_pending = false;
         qemu_plugin_raise_exception(ii->vaddr, cause, tval);       /* does not return */
      };
   const char *cls = rvisa::info(rvisa::class_of(ii->op)).name;
   if (strncmp(cls, "acc_", 4) == 0)
      acc::execute(v->acc_backend, ii->op, rvisa::word_of((const uint8_t *)&ii->word, 4), env);
   else
      ame::execute(v->ame_backend, ii->op, rvisa::word_of((const uint8_t *)&ii->word, 4), env);
   if (ii->claimed)
      __sync_fetch_and_add(&g_exec_claimed, 1);       /* the block continues past it */
   else
   {
      __sync_fetch_and_add(&g_exec_trapped, 1);
      qemu_plugin_set_pc(ii->vaddr + ii->size);       /* before QEMU's illegal-instruction code runs */
   }
}

/* The translation-time claim: an instruction QEMU cannot decode is one of the
 * plugin models' if the generated decoder knows it as an ame_* or acc_*
 * class.  Only the mark is made here; tb_trans registers the callbacks in
 * their order (the tracer's first, then the model's). */
bool insn_claim(unsigned int cpu_index, struct qemu_plugin_insn *insn, void *)
{
   if (g_arch != ARCH_RISCV || !opt_claim)
      return false;
   uint8_t bytes[8] = {0};
   const size_t got = qemu_plugin_insn_data(insn, bytes, sizeof(bytes));
   const unsigned length = got ? rvisa::length_of(bytes) : 0;
   if (!length || length > got)
      return false;
   const unsigned op = rvisa::decode(rvisa::word_of(bytes, length), length);
   if (!op)
      return false;
   const char *cls = rvisa::info(rvisa::class_of(op)).name;
   if (strncmp(cls, "ame_", 4) != 0 && strncmp(cls, "acc_", 4) != 0)
      return false;
   std::lock_guard<std::mutex> lock(g_claim_lock);
   g_claimed.insert(insn);
   return true;
}

/* ------------------------------------------------- the AME unit's registers
 * The seven CSRs, the datatype registers and the M and accumulator squares of
 * the plugin's AME model, in the vcpu's gdbstub register list, so gdb shows
 * them and qemu_plugin_read/write_register reach them. */
namespace
{
   const unsigned AME_NCSR = 7;
   const char *const ame_csr_names[AME_NCSR] = { "amenlen", "ameudsz", "amestype", "ameown", "amefflags", "amexsat", "amestatus" };
   const unsigned ame_csr_addr[AME_NCSR] = { ame::CSR_AMENLEN, ame::CSR_AMEUDSZ, ame::CSR_AMESTYPE, ame::CSR_AMEOWN, ame::CSR_AMEFFLAGS, ame::CSR_AMEXSAT, ame::CSR_AMESTATUS };
   std::vector<std::string> ame_reg_names;      /* the storage the gdbstub keeps pointers into */
   std::vector<const char *> ame_reg_name_ptrs, ame_reg_types;
   std::vector<unsigned> ame_reg_bits;
   std::string ame_reg_tags;

   /* register index -> which state */
   enum { R_CSR = 0, R_MD = R_CSR + AME_NCSR, R_AD = R_MD + ame::NUM_M, R_M = R_AD + ame::NUM_ACC, R_ACC = R_M + ame::NUM_M, R_END = R_ACC + ame::NUM_ACC };

   void ame_reg_read(unsigned int cpu_index, unsigned int reg, GByteArray *buf, void *)
   {
      VCPU *v = vcpu_get(cpu_index);
      const ame::Backend &b = v->ame_backend;
      const unsigned sq = ame::square_n * ame::square_n * 4;
      if (reg < R_MD) { uint64_t x = ame::csr_read(b, ame_csr_addr[reg]); g_byte_array_append(buf, (const guint8 *)&x, 8); }
      else if (reg < R_AD) g_byte_array_append(buf, (const guint8 *)&b.Md[reg - R_MD], 4);
      else if (reg < R_M) g_byte_array_append(buf, (const guint8 *)&b.Ad[reg - R_AD], 4);
      else if (reg < R_ACC) g_byte_array_append(buf, (const guint8 *)b.M[reg - R_M], sq);
      else if (reg < R_END) g_byte_array_append(buf, (const guint8 *)b.Acc[reg - R_ACC], sq);
   }
   void ame_reg_write(unsigned int cpu_index, unsigned int reg, const uint8_t *data, void *)
   {
      VCPU *v = vcpu_get(cpu_index);
      ame::Backend &b = v->ame_backend;
      const unsigned sq = ame::square_n * ame::square_n * 4;
      if (reg < R_MD) { uint64_t x; memcpy(&x, data, 8); ame::csr_write(b, ame_csr_addr[reg], x); }
      else if (reg < R_AD) memcpy(&b.Md[reg - R_MD], data, 4);
      else if (reg < R_M) memcpy(&b.Ad[reg - R_AD], data, 4);
      else if (reg < R_ACC) memcpy(b.M[reg - R_M], data, sq);
      else if (reg < R_END) memcpy(b.Acc[reg - R_ACC], data, sq);
   }
   uint64_t ame_csr_cb_read(unsigned int cpu_index, unsigned int csr, void *) { return ame::csr_read(vcpu_get(cpu_index)->ame_backend, csr); }
   void ame_csr_cb_write(unsigned int cpu_index, unsigned int csr, uint64_t val, void *) { ame::csr_write(vcpu_get(cpu_index)->ame_backend, csr, val); }

   void ame_register_state(VCPU *v, unsigned cpu_index)
   {
      if (ame_reg_names.empty())
      {
         const unsigned nelem = ame::square_n * ame::square_n;
         char tag[96]; snprintf(tag, sizeof(tag), "<vector id=\"ame_sq\" type=\"uint32\" count=\"%u\"/>", nelem);
         ame_reg_tags = tag;
         for (unsigned i = 0; i < AME_NCSR; i++) { ame_reg_names.push_back(ame_csr_names[i]); ame_reg_bits.push_back(64); ame_reg_types.push_back("uint64"); }
         for (unsigned i = 0; i < ame::NUM_M; i++) { ame_reg_names.push_back("md" + std::to_string(i)); ame_reg_bits.push_back(32); ame_reg_types.push_back("uint32"); }
         for (unsigned i = 0; i < ame::NUM_ACC; i++) { ame_reg_names.push_back("ad" + std::to_string(i)); ame_reg_bits.push_back(32); ame_reg_types.push_back("uint32"); }
         for (unsigned i = 0; i < ame::NUM_M; i++) { ame_reg_names.push_back("m" + std::to_string(i)); ame_reg_bits.push_back(nelem * 32); ame_reg_types.push_back("ame_sq"); }
         for (unsigned i = 0; i < ame::NUM_ACC; i++) { ame_reg_names.push_back("acc" + std::to_string(i)); ame_reg_bits.push_back(nelem * 32); ame_reg_types.push_back("ame_sq"); }
         for (const std::string &n : ame_reg_names) ame_reg_name_ptrs.push_back(n.c_str());
      }
      if (SNP_HAVE_REGISTERS)
         v->pregs = qemu_plugin_register_vcpu_registers(cpu_index, "org.riscv.ame", ame_reg_tags.c_str(), ame_reg_name_ptrs.data(), ame_reg_bits.data(),
                                                        ame_reg_types.data(), (unsigned)ame_reg_names.size(), ame_reg_read, ame_reg_write, nullptr);
      static bool csrs_claimed = false;
      if (SNP_HAVE_SYSREGS && !csrs_claimed)
      {
         csrs_claimed = true;
         unsigned ok = 0;
         for (unsigned i = 0; i < AME_NCSR; i++)
            ok += qemu_plugin_register_sysreg_cb(ame_csr_addr[i], ame_csr_cb_read, ame_csr_cb_write, nullptr) ? 1 : 0;
         if (ok != AME_NCSR)
            fprintf(stderr, "[SNIPER-QEMU] only %u of %u AME CSRs could be claimed\n", ok, AME_NCSR);
      }
   }
}

void insn_exec(unsigned int cpu_index, void *udata)
{
   const InsnInfo *ii = (const InsnInfo *)udata;
   if (!g_tracing.load(std::memory_order_relaxed))
      return;                      /* outside the ROI */
   VCPU *v = vcpu_get(cpu_index);

   /* The previous instruction's fate is now known. */
   flush_pending(v, ii->vaddr);


   v->pend_addr   = ii->vaddr;
   v->pend_paddr  = ii->paddr;
   v->pend_paddr2 = ii->paddr2;
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

/* A magic instruction (is_magic): send it to the simulator, which acts on it
 * and, when there is a response channel, answers -- the answer goes into the
 * result register before the instruction runs, and the instruction itself
 * leaves that register alone.  Registered after insn_exec, with RW_REGS, so
 * the instruction is pending when this runs and goes out ahead of the magic
 * record, as with the SDE recorder.
 *
 * With roi, ROI start turns tracing on and ROI end turns it off again; the
 * markers are sent in both directions, so the simulator switches its models
 * at the same instruction. */
uint64_t read_reg(struct qemu_plugin_register *r, bool have)
{
   if (!have)
      return 0;
   GByteArray *b = g_byte_array_new();
   uint64_t val = 0;
   if (qemu_plugin_read_register(r, b) && b->len)
      memcpy(&val, b->data, std::min<size_t>(b->len, sizeof(val)));
   g_byte_array_free(b, TRUE);
   return val;
}

void find_magic_registers(VCPU *v)
{
   /* i386: arg0 is edx, not ebx -- sim_api.h keeps ebx for -fPIC code */
   static const char *const names[3][4][2] = {
      /* answer        command        arg0           arg1 */
      { {"rax","eax"}, {"rax","eax"}, {"rbx","edx"}, {"rcx","ecx"} },   /* x86 */
      { {"x0",nullptr}, {"x1",nullptr}, {"x2",nullptr}, {"x3",nullptr} }, /* AArch64 */
      { {"a0","x10"}, {"a0","x10"}, {"a1","x11"}, {"a2","x12"} },       /* RISC-V */
   };
   const unsigned a = g_arch == ARCH_X86 ? 0 : g_arch == ARCH_AARCH64 ? 1 : 2;
   GArray *regs = qemu_plugin_get_registers();
   for (guint i = 0; regs && i < regs->len; i++)
   {
      qemu_plugin_reg_descriptor *d = &g_array_index(regs, qemu_plugin_reg_descriptor, i);
      for (unsigned k = 0; k < 4; k++)
         for (unsigned n = 0; n < 2; n++)
            if (!v->mreg_have[k] && names[a][k][n] && strcmp(d->name, names[a][k][n]) == 0)
            {
               /* QEMU encodes a handle as (gdb register number << 1) | read
                * only, so the first register's is null: found is kept apart */
               v->mreg[k] = d->handle;
               v->mreg_have[k] = true;
            }
   }
   if (regs)
      g_array_free(regs, TRUE);
   for (unsigned k = 0; k < 4; k++)
      if (!v->mreg_have[k])
         fprintf(stderr, "[SNIPER-QEMU] no register handle for magic operand %u (%s): it reads as 0\n",
                 k, names[a][k][0]);
   v->mreg_found = true;
}

void magic_exec(unsigned int cpu_index, void *udata)
{
   const InsnInfo *ii = (const InsnInfo *)udata;
   VCPU *v = vcpu_get(cpu_index);
   if (!v->writer || v->closed)
      return;
   if (!v->mreg_found)
      find_magic_registers(v);

   const uint64_t cmd  = read_reg(v->mreg[1], v->mreg_have[1]);
   const uint64_t arg0 = read_reg(v->mreg[2], v->mreg_have[2]);
   const uint64_t arg1 = read_reg(v->mreg[3], v->mreg_have[3]);

   if (opt_roi && cmd == SIM_CMD_ROI_START && !g_tracing.load())
   {
      g_tracing.store(true);
      if (opt_verbose)
         fprintf(stderr, "[SNIPER-QEMU] ROI begin\n");
   }

   /* The magic instruction goes out first (when traced), then the record. */
   flush_pending(v, ii->vaddr + ii->size);
   const uint64_t res = v->writer->Magic(cmd, arg0, arg1);

   if (opt_roi && cmd == SIM_CMD_ROI_END && g_tracing.load())
   {
      g_tracing.store(false);
      if (opt_verbose)
         fprintf(stderr, "[SNIPER-QEMU] ROI end\n");
   }

   if (opt_response && res != cmd && v->mreg_have[0])
   {
      /* As wide as the register reads: 4 bytes on i386 */
      GByteArray *b = g_byte_array_new();
      size_t width = 8;
      if (qemu_plugin_read_register(v->mreg[0], b) && b->len && b->len < width)
         width = b->len;
      g_byte_array_set_size(b, 0);
      g_byte_array_append(b, (const guint8 *)&res, (guint)width);
      if (!qemu_plugin_write_register(v->mreg[0], b))
         fprintf(stderr, "[SNIPER-QEMU] could not write the answer to magic command %" PRIu64
                         ": the guest reads its command back (SimInSimulator() says no)\n", cmd);
      g_byte_array_free(b, TRUE);
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
   /* The previous block's last instruction is still pending (emission waits
    * for the successor, to know whether a branch was taken), and it ran in
    * the old mode: emit it first, with this block's start as its successor.
    * Otherwise it is recorded after the ISAChange and decoded in the new
    * mode -- the far jump that enters long mode, 16-bit code, was decoded
    * as 64-bit and the simulation aborted. */
   if (v->writer)
      flush_pending(v, ti->vaddr);
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
   /* System mode: the physical address QEMU just used.  Device (MMIO)
    * accesses report an offset into the device, not an address; they get
    * none, and the simulator keeps the virtual one for them. */
   uint64_t paddr = 0;
   if (in_system_mode)
   {
      struct qemu_plugin_hwaddr *hw = qemu_plugin_get_hwaddr(info, vaddr);
      if (hw && qemu_plugin_hwaddr_is_io(hw))
         paddr = PADDR_IO;
      else if (hw)
         paddr = qemu_plugin_hwaddr_phys_addr(hw);
      /* An access that crosses into the next page: that page's frame too,
       * which need not be the next one */
      const uint64_t size = 1u << qemu_plugin_mem_size_shift(info);
      const uint64_t next = (vaddr + size - 1) & ~0xfffULL;
      uint64_t pa2 = 0;
      if (next != (vaddr & ~0xfffULL) && qemu_plugin_translate_vaddr(next, &pa2))
         v->pend_more_pages.push_back(std::make_pair(next, pa2));
   }
   if (v->pend_addrs.size() < Sift::MAX_DYNAMIC_ADDRESSES)
   {
      v->pend_addrs.push_back(vaddr);
      v->pend_paddrs.push_back(paddr);
   }
   if (v->pend_footprint)
   {
      VCPU::Access acc;
      acc.addr  = vaddr;
      acc.paddr = paddr;
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
   if (!opt_response || !g_tracing.load(std::memory_order_relaxed))
      return;

   /* Close out the instruction that issued the syscall first. */
   flush_pending(v, 0);

   const uint64_t args[6] = { a1, a2, a3, a4, a5, a6 };
   v->writer->Syscall((uint16_t)num, (const char *)args, sizeof(args));
}

void vcpu_init(SNP_VCPU_ARGS)
{
   VCPU *v = vcpu_get(cpu_index);
   if (g_arch == ARCH_RISCV)
      ame_register_state(v, cpu_index);

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
      if (!opt_trace)
      return;                      /* a functional run: no trace file */
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
      if (in_system_mode)
      {
         /* Where the code is in physical memory: what identifies it in a
          * full-system trace (Writer::Mapping, CodePhysical) */
         uint64_t pa = 0;
         if (qemu_plugin_translate_vaddr(ii->vaddr, &pa))
            ii->paddr = pa;
         const uint64_t last = ii->vaddr + (got ? got : 1) - 1;
         if ((last & ~0xfffULL) != (ii->vaddr & ~0xfffULL) && qemu_plugin_translate_vaddr(last & ~0xfffULL, &pa))
            ii->paddr2 = pa;
      }
      if (got && !in_system_mode)
         Sift::CodeTracker::observe(ii->vaddr, bytes, (uint32_t)got);
      else if (got && ii->paddr)
      {
         /* Code rewritten in place, by physical page, the writer's key; one
          * piece per page an instruction spans, since the next virtual page
          * need not be the next physical one */
         const uint64_t first = std::min<uint64_t>(got, 0x1000 - (ii->vaddr & 0xfff));
         Sift::CodeTracker::observe(ii->paddr | Sift::CODE_KEY_PHYSICAL, bytes, (uint32_t)first);
         if (first < got && ii->paddr2)
            Sift::CodeTracker::observe((ii->paddr2 & ~0xfffULL) | Sift::CODE_KEY_PHYSICAL, bytes + first, (uint32_t)(got - first));
      }
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
               ii->ame = ii->op && (strncmp(cls, "ame_", 4) == 0 || strncmp(cls, "acc_", 4) == 0);   /* executed by a plugin model */
            }
         }
      }
      else if (g_arch == ARCH_AARCH64)
      {
         ii->is_branch = got ? aarch64_is_conditional_branch(bytes, got) : false;
         ii->mode_mask = 0;
      }
      else
      {
         ii->is_branch = got ? xed_is_conditional_branch(bytes, got) : false;
         ii->mode_mask = got ? mode_mask_for(bytes, got, ii->size) : 0;
      }
      if (ii->mode_mask)
         ti->mode_mask &= ii->mode_mask;  /* one mode governs the whole block */
      ii->magic = got && is_magic(bytes, got);

      if (ii->ame)
      {
         std::lock_guard<std::mutex> lock(g_claim_lock);
         ii->claimed = g_claimed.erase(insn) > 0;
      }
      if (!opt_trace)
      {
         if (ii->ame)
            qemu_plugin_register_vcpu_insn_exec_cb(insn, ame_exec, ii->claimed ? QEMU_PLUGIN_CB_RW_REGS : QEMU_PLUGIN_CB_RW_REGS_PC, ii);
         continue;
      }
      qemu_plugin_register_vcpu_insn_exec_cb(insn, insn_exec,
                                             QEMU_PLUGIN_CB_NO_REGS, ii);
      if (ii->magic)   /* after the tracer's callback: the instruction is pending */
         qemu_plugin_register_vcpu_insn_exec_cb(insn, magic_exec,
                                                QEMU_PLUGIN_CB_RW_REGS, ii);
      if (ii->ame)     /* after the tracer's callback: the record is pending when the model runs */
         qemu_plugin_register_vcpu_insn_exec_cb(insn, ame_exec, ii->claimed ? QEMU_PLUGIN_CB_RW_REGS : QEMU_PLUGIN_CB_RW_REGS_PC, ii);
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
   if (g_ndropped_lines)
      fprintf(stderr, "[SNIPER-QEMU] %" PRIu64 " cache lines touched beyond the spans their records hold (counted, not timed)\n", g_ndropped_lines);
   if (g_exec_claimed || g_exec_trapped)
      fprintf(stderr, "[SNIPER-QEMU] plugin-emulated instructions: %" PRIu64 " claimed at translation, %" PRIu64 " through the exception path\n", g_exec_claimed, g_exec_trapped);
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
   else if (!strcmp(info->target_name, "aarch64"))
      g_arch = ARCH_AARCH64;
   else
   {
      fprintf(stderr, "[SNIPER-QEMU] unsupported target %s (x86_64, i386, riscv32, riscv64, aarch64)\n",
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
      else if (key == "ame_n")                                   /* the AME square side (4, 8, 16, 32) */
      {
         const unsigned n = (unsigned)strtoul(val.c_str(), nullptr, 0);
         if (n < 2 || n > ame::MAXN || (n & (n - 1)))
         {
            fprintf(stderr, "[SNIPER-QEMU] ame_n must be a power of two between 2 and %u\n", ame::MAXN);
            return -1;
         }
         ame::square_n = n;
      }
      else if (key == "smc")      opt_smc = val != "0" && val != "off" && val != "false";     /* smc=0: send each code page once, as before */
      else if (key == "claim")    opt_claim = val != "0" && val != "off" && val != "false";   /* claim=0: emulate via the exception path even when the QEMU can claim */
      else if (key == "trace")    opt_trace = val != "0" && val != "off" && val != "false";   /* trace=0: a functional run, the models only */
      /* A bare key is on (the short form QEMU now warns about); key=on/off
       * otherwise, so that response=off is not taken for response. */
      else if (key == "verbose")  opt_verbose = !eq || (val != "0" && val != "off" && val != "false");
      else if (key == "response") opt_response = !eq || (val != "0" && val != "off" && val != "false");
      else if (key == "roi")      opt_roi = val != "0" && val != "off" && val != "false";
      else if (key == "app")      opt_app = (unsigned)strtoul(val.c_str(), nullptr, 0);
      else if (key == "time")     opt_time = true;
      else if (key == "ns")       opt_ns_per_insn = strtoull(val.c_str(), nullptr, 0);
      else
      {
         fprintf(stderr, "[SNIPER-QEMU] unknown option '%s'\n", argv[i]);
         return -1;
      }
   }

   Sift::CodeTracker::enable(opt_smc);
   g_tracing.store(!opt_roi);

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
#if QEMU_PLUGIN_VERSION >= 7
   if (SNP_HAVE_CLAIM)
      qemu_plugin_register_vcpu_insn_claim_cb(id, insn_claim, nullptr);
#endif
   SNP_REG_VCPU_EXIT(id, vcpu_exit);
   SNP_REG_ATEXIT(id, at_exit);
   return 0;
}
