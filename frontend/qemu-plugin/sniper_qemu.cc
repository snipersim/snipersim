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

#include <glib.h>

#include "qemu_compat.h"

extern "C" {
#include <xed-interface.h>
}

#include "sift_writer.h"
#include "sift_format.h"

extern "C" {
QEMU_PLUGIN_EXPORT int qemu_plugin_version = QEMU_PLUGIN_VERSION;
}

namespace {

/* ---------------------------------------------------------------- options */

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

/* Filled once per translation, shared by every execution of that instruction. */
struct InsnInfo
{
   uint64_t vaddr;
   uint8_t  size;
   bool     is_branch;
};

/* ------------------------------------------------------------- per-vCPU */

struct VCPU
{
   Sift::Writer *writer   = nullptr;
   bool          closed   = false;
   uint64_t      icount   = 0;
   uint64_t      flow_next = 0;

   /* The instruction waiting for its successor, with the addresses its memory
    * callbacks have reported so far. */
   bool     have_pending = false;
   uint64_t pend_addr    = 0;
   uint8_t  pend_size    = 0;
   bool     pend_branch  = false;
   std::vector<uint64_t> pend_addrs;
};

std::vector<VCPU *> vcpus;

VCPU *vcpu_get(unsigned int idx)
{
   if (idx >= vcpus.size())
      vcpus.resize(idx + 1, nullptr);
   if (!vcpus[idx])
      vcpus[idx] = new VCPU();
   return vcpus[idx];
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
   if (opt_verbose)
      fprintf(stderr, "[SNIPER-QEMU] vcpu %u -> %s\n", idx, filename);
}

/* Emit the buffered instruction, now that we know where control went next. */
void flush_pending(VCPU *v, uint64_t next_vaddr)
{
   if (!v->have_pending)
      return;

   const bool taken = v->pend_branch &&
                      (next_vaddr != v->pend_addr + v->pend_size);

   v->writer->Instruction(v->pend_addr, v->pend_size,
                          (uint8_t)v->pend_addrs.size(),
                          v->pend_addrs.data(),
                          v->pend_branch, taken,
                          false /* is_predicate */, true /* executed */);

   v->have_pending = false;
   v->pend_addrs.clear();
   v->icount++;
}

}  /* namespace */

namespace {

/* ------------------------------------------------------------ callbacks */

void insn_exec(unsigned int cpu_index, void *udata)
{
   const InsnInfo *ii = (const InsnInfo *)udata;
   VCPU *v = vcpu_get(cpu_index);

   /* The previous instruction's fate is now known. */
   flush_pending(v, ii->vaddr);

   v->pend_addr   = ii->vaddr;
   v->pend_size   = ii->size;
   v->pend_branch = ii->is_branch;
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

void mem_access(unsigned int cpu_index, qemu_plugin_meminfo_t info,
                uint64_t vaddr, void *udata)
{
   VCPU *v = vcpu_get(cpu_index);
   if (v->have_pending && v->pend_addrs.size() < Sift::MAX_DYNAMIC_ADDRESSES)
      v->pend_addrs.push_back(vaddr);
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
   if (!v->writer)
      writer_open(v, cpu_index);

   /* A second guest thread in user mode is the configuration that deadlocks:
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

   for (size_t i = 0; i < n; i++)
   {
      struct qemu_plugin_insn *insn = qemu_plugin_tb_get_insn(tb, i);

      /* One record per static instruction, reused by every execution. */
      InsnInfo *ii = new InsnInfo();
      ii->vaddr = qemu_plugin_insn_vaddr(insn);
      ii->size  = (uint8_t)qemu_plugin_insn_size(insn);

      /* Decoded once per translation, so the cost is amortised over every
       * execution of the block. */
      uint8_t bytes[XED_MAX_INSTRUCTION_BYTES] = {0};
      size_t got = qemu_plugin_insn_data(insn, bytes, sizeof(bytes));
      ii->is_branch = got ? xed_is_conditional_branch(bytes, got) : false;

      qemu_plugin_register_vcpu_insn_exec_cb(insn, insn_exec,
                                             QEMU_PLUGIN_CB_NO_REGS, ii);
      qemu_plugin_register_vcpu_mem_cb(insn, mem_access,
                                       QEMU_PLUGIN_CB_NO_REGS,
                                       QEMU_PLUGIN_MEM_RW, nullptr);
   }
}

void at_exit(SNP_UDATA_ARGS)
{
   uint64_t total = 0;
   for (size_t i = 0; i < vcpus.size(); i++)
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
