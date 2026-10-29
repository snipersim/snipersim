/*
 * Sniper frontend as a QEMU TCG plugin.
 *
 * Loaded into QEMU with -plugin.  Writes SIFT, so to Sniper it is just another
 * trace producer.  A stock QEMU 11 runs it; the patches in tools/ add calls
 * it uses when present (bound weakly, see qemu_plugin_ext.h).
 *
 * Two modes, because QEMU offers two and they have different rules:
 *
 *   user mode    qemu-x86_64, qemu-riscv64, qemu-aarch64.  One guest thread
 *                per host thread, one SIFT stream each.  Flow control waits
 *                for Sniper's answer, and a vCPU cannot wait inside a
 *                callback: it still counts as running there, and anything
 *                that needs an exclusive section (a code-cache flush, an
 *                atomic another thread is emulating) waits for it -- a
 *                deadlock as soon as the guest has two threads.  With a QEMU
 *                that has qemu_plugin_vcpu_wait_at()
 *                (tools/qemu-11.1.1-plugin-wait.patch), every wait leaves the
 *                run loop first, the way a system call does, and
 *                multi-threaded guests run; see "waiting" below.  Without it,
 *                the wait happens in the callback and a second guest thread
 *                is refused on a live channel rather than deadlocked (to
 *                files, without flow control, threads are recorded either
 *                way; see test/qemu-plugin-probe).
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

#include <stdarg.h>
#include <unistd.h>
#include <pthread.h>
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
#include <condition_variable>
#include <mutex>
#include <unordered_set>
#include <sys/syscall.h>
#include <linux/futex.h>

#include "sift_writer.h"
#include "sim_api.h"         /* the SIM_CMD_ numbers */
#include "pacsim_qemu.h"      /* Pac-Sim: the sampler behind the plugin */
#include "sift_format.h"
#include "guest_time.h"        /* timeouts in the guest's time */

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
/* Serialises closing the streams.  A worker thread's exit clears its tid
 * word and wakes the joiner (linux-user's exit) before QEMU runs our
 * vcpu_exit for it; the joiner can then reach exit_group, and at_exit, while
 * the worker is still inside writer_close.  Both ended the same writer, and
 * at_exit deleted it under the worker: a crash in deflate at exit, one
 * recording in thirty (testing/test_qemu.py aarch64-user-mt). */
std::mutex  g_close_lock;
std::unordered_set<const struct qemu_plugin_insn *> g_claimed;   /* claimed in this translation, consumed by tb_trans */

std::string opt_output   = "sniper";
uint64_t    opt_flow     = 1000;   /* matches KnobFlowControl in sift/recorder */
bool        opt_verbose  = false;
bool        opt_response = false;  /* user mode: blocking flow control */
/* The rate of RISC-V's time CSR, which Linux learns from the device tree
 * and QEMU's user mode does not define: QEMU's virt board's 10 MHz */
uint64_t    opt_timebase = 10000000;
/* The guest's time zero, seconds since the epoch: Sniper's osemu/time_start
 * default, so that a clock the plugin answers itself (no simulator to ask)
 * starts where the simulator's does (sift/sift_format.h) */
uint64_t    opt_time_start = Sift::TIME_START_DEFAULT;
/* Instructions each vCPU has executed, in every mode, counted inline at each
 * block's entry: the one-IPC model's clock where the simulator does not
 * answer a time read (Sift::one_ipc_ns) */
struct qemu_plugin_scoreboard *g_icount_sb = nullptr;
qemu_plugin_u64 g_icount;
/* roi: trace only between SimRoiStart() and SimRoiEnd(), as the SDE and
 * DynamoRIO front ends do with -roi.  The magic instructions themselves are
 * always sent, so the simulator sees the markers either way. */
bool        opt_roi      = false;
/* The recording mode: what each executed instruction becomes.  Detailed is
 * the Instruction record with its addresses; memory-only is CacheOnly
 * records for accesses and branches with the instruction count riding
 * along, which keeps the simulator's caches and predictors warm; counting
 * sends only instruction counts (a live channel: the simulator's answer to
 * each count says which mode comes next, and that is how it and the Pac-Sim
 * sampler steer the recording).  Off records nothing and asks nothing: a
 * trace written to a file, outside its ROI.  Ended: the ROI is over on a
 * live channel, the simulator was told the application exited. */
enum { MODE_DETAILED = 0, MODE_MEMORY = 1, MODE_ICOUNT = 2, MODE_OFF = 3 };
std::atomic<int> g_mode(MODE_DETAILED);
bool        g_in_roi     = false;
unsigned    opt_app      = 0;      /* the app number in the file names (record-trace -s) */
bool        opt_time     = false;  /* system mode: drive guest time */
uint64_t    opt_ns_per_insn = 1;   /* until the timing model answers */

bool in_system_mode = false;
uint64_t getcode_calls = 0, getcode_fail = 0;
const void *time_handle = nullptr;

/* ---------------------------------------------------------------- waiting
 *
 * User mode with a response channel, on a QEMU that can wait: no callback
 * waits for Sniper where it fires.  A callback that would (flow control, a
 * magic instruction, a sampler event that ends a region) records what it
 * needs, marks its stage done and leaves the run loop with
 * qemu_plugin_vcpu_wait_at(); wait_body() then runs on the same host thread
 * with the vCPU no longer running, does the waiting, and execution resumes
 * at the instruction the callback belonged to.  (System calls need none of
 * this: QEMU calls the plugin for them from its cpu loop, outside the run
 * loop already.)
 *
 * Resuming re-runs that instruction's callbacks, and in a fresh block
 * starting there, whose block-level callbacks run too.  The stage mask says
 * what already happened at this address: every callback is a stage, a
 * stage that ran is not run again, and the mask is dropped when the next
 * instruction (or, on a fresh visit, the next block) begins.  A block
 * begun by a resume was counted as part of the block the wait interrupted,
 * so its block-level stages are marked done as well (unless the wait was
 * theirs: then the mask already says which ran). */
bool        g_wait = false;
bool        g_counters = false;   /* the guest's counter reads are answered with Sniper's clocks */
enum { S_TB_COUNT = 1, S_TB_FLOW = 2, S_INSN = 4, S_MAGIC = 8, S_SYNC = 16, S_BACKEDGE = 32, S_AME = 64, S_COUNTER = 128 };
enum { W_NONE = 0, W_TB_COUNT, W_FLOW, W_MAGIC, W_SYNC, W_BACKEDGE, W_COUNTER };

/* Threads.  Numbered in the order they start, like the recorder's, and that
 * is the number in the stream's file name: Sniper opens app0.th<N> when it
 * has seen N NewThread records.  (A vCPU index is not that number: QEMU
 * gives a thread started after another ended the ended one's index.)  A
 * clone's child tid pointer is queued by the parent's clone and taken by the
 * next thread to start, for the futex wake that lets the joiner go at its
 * exit (the kernel's own wake goes to the kernel, not to Sniper's model of
 * the futex), with the parent's vCPU, whose time the child's clock starts
 * from (the one-IPC model, guest_time.h). */
std::atomic<unsigned> g_num_threads(0);
std::atomic<bool>     g_main_started(false);
std::mutex            g_tid_lock;
struct Clone { uint64_t tid_ptr; int parent; };
std::vector<Clone>    g_tid_ptrs;
/* Sniper creates a thread's channels when it hears NewThread, which the
 * parent sends when its clone returns -- and the child may be running by
 * then.  It waits, in its init hook (outside the run loop), until as many
 * threads have been announced as have started before it, then opens what
 * Sniper has created -- or nothing, when its parent's clone returned after
 * the ROI (no NewThread was sent; g_announce_sent says which). */
std::condition_variable g_announce_cv;
std::vector<bool>     g_announce_sent;        /* under g_tid_lock: announcement N-1 sent NewThread */

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
bool is_counter_read(const uint8_t *bytes, size_t len);

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

/* An instruction that reads a clock or counter QEMU's user mode answers from
 * the host, and the plugin from Sniper (qemu_plugin_register_counter_cb):
 * RISC-V CSR instructions on time, cycle and instret (and their high
 * halves); AArch64 MRS of CNTVCT_EL0, CNTVCTSS_EL0 and PMCCNTR_EL0; x86
 * rdtsc and rdtscp. */
bool is_counter_read(const uint8_t *bytes, size_t len)
{
   if (g_arch == ARCH_RISCV)
   {
      if (len < 4 || (bytes[0] & 0x7f) != 0x73)
         return false;
      const uint32_t w = bytes[0] | bytes[1] << 8 | bytes[2] << 16 | (uint32_t)bytes[3] << 24;
      const unsigned funct3 = (w >> 12) & 7, csr = w >> 20;
      return funct3 != 0 && funct3 != 4 &&
             (csr == 0xc00 || csr == 0xc01 || csr == 0xc02 || csr == 0xc80 || csr == 0xc81 || csr == 0xc82);
   }
   if (g_arch == ARCH_AARCH64)
   {
      if (len < 4)
         return false;
      const uint32_t w = bytes[0] | bytes[1] << 8 | bytes[2] << 16 | (uint32_t)bytes[3] << 24;
      if ((w & 0xfff00000) != 0xd5300000)   /* MRS of an op0=3 system register */
         return false;
      const unsigned sysreg = (w >> 5) & 0x7fff;   /* o0:op1:CRn:CRm:op2 */
      return sysreg == 0x5f02 || sysreg == 0x5f06 || sysreg == 0x5ce8;
   }
   size_t k = 0;   /* x86: past the legacy and REX prefixes */
   while (k < len && (bytes[k] == 0x66 || bytes[k] == 0xf2 || bytes[k] == 0xf3 || (bytes[k] & 0xf0) == 0x40))
      k++;
   return (k + 1 < len && bytes[k] == 0x0f && bytes[k + 1] == 0x31) ||
          (k + 2 < len && bytes[k] == 0x0f && bytes[k + 1] == 0x01 && bytes[k + 2] == 0xf9);
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
   bool     counter = false;    /* reads a clock or counter the simulator answers (is_counter_read) */
   uint32_t tb_left = 0;        /* instructions from this one to its block's end: the block's count is added at its entry */
   bool     is_jump = false;    /* an unconditional direct jump (x86 jmp rel, RISC-V jal x0 / c.j, AArch64 b): a loop's closing jump when backward, not a SIFT branch */
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
   /* Between measured regions: instructions counted but not yet reported,
    * and the previous instruction if it was a branch whose outcome the next
    * instruction settles (a CacheOnly branch record) */
   uint32_t      ff_pending = 0;
   bool          ff_prev_branch = false;
   uint64_t      ff_prev_addr = 0;
   uint8_t       ff_prev_size = 0;
   /* a synchronisation point's call site: the stack pointer (x86, the
    * return address is on the stack) or the return-address register */
   struct qemu_plugin_register *reg_ret = nullptr;
   bool          reg_ret_have = false;

   int           insn_mode = MODE_OFF;   /* the recording mode of the instruction in flight */
   bool          hold_pending = false;   /* the pending instruction has not executed yet: a request must not flush it */

   /* threads (user mode) */
   unsigned      thread_num = 0;        /* the stream's number: app0.th<thread_num> */
   uint64_t      tid_ptr    = 0;        /* CLONE_CHILD_CLEARTID's word, woken for the joiner at exit */
   bool          threadinfo_pending = false;   /* the tid goes with the first system call on the channel */
   bool          clone_pending = false; /* a thread clone in flight: announced at its return */
   uint64_t      clone_tidptr = 0;

   /* waiting (see "waiting" above) */
   uint64_t      done_vaddr = 0;
   unsigned      done_mask  = 0;
   bool          resuming   = false;    /* the next block begins at a wait's instruction */
   bool          wait_tb_level = false; /* the wait was a block-level stage's */
   int           wait_kind  = W_NONE;
   uint64_t      wait_vaddr = 0;
   uint32_t      wait_n     = 0;        /* W_TB_COUNT: the block's instruction count */
   uint64_t      wait_site  = 0;        /* W_SYNC: the call site */
   uint64_t      wait_a[3]  = {0, 0, 0};/* W_MAGIC: the command and its arguments */
   const InsnInfo *wait_ii  = nullptr;  /* W_MAGIC: the instruction */
   unsigned      vcpu_index = 0;        /* its index (user mode: the thread's slot, reused after it ends) */
   /* Timed calls (guest_time.h): the thread's scratch page in the guest
    * (timeouts of the plugin's own, never the guest's), a wait interrupted
    * by a signal and restarted (its deadline), a call in which the thread
    * counts as waiting (the one-IPC model) */
   uint64_t      scratch    = 0;
   bool          restart_valid = false;
   int64_t       restart_num = 0;
   uint64_t      restart_args[6] = {0, 0, 0, 0, 0, 0};
   uint64_t      restart_deadline = 0;
   bool          clock_waiting = false;
   /* The simulator's clocks for the counter read in flight (counter_exec),
    * taken by the counter callback QEMU calls within the instruction */
   bool          ctr_valid  = false;
   uint64_t      icount_base = 0;   /* g_icount at the thread's start: its instructions are counted from there */
   uint64_t      ctr_time_ns = 0, ctr_core_cycles = 0, ctr_global_cycles = 0, ctr_instructions = 0;
};

/* One slot per vCPU, sized once and never resized.
 *
 * In user mode a guest thread is a host thread and its callbacks run on it, so
 * several vCPUs call in at the same time.  Growing a shared std::vector from
 * those callbacks raced and crashed the moment a guest started a second
 * thread.  A fixed table of pointers avoids it without locking the hot path:
 * a slot is only ever written by the vCPU it belongs to, so nothing needs to
 * be shared. */
/* The plugin cannot go on: say why, leave the reason beside the trace
 * (<output>.app<N>.refused) and end QEMU.  QEMU's exit status is the guest's,
 * so status 1 alone could not tell run-sniper a refusal from a guest that
 * exits 1, and the partial simulation passed as a run; the file is what
 * run-sniper looks for. */
static void refuse(const char *fmt, ...) __attribute__((noreturn, format(printf, 1, 2)));
static void refuse(const char *fmt, ...)
{
   char msg[2048];
   va_list ap;
   va_start(ap, fmt);
   vsnprintf(msg, sizeof(msg), fmt, ap);
   va_end(ap);
   fputs(msg, stderr);
   char path[4096];
   snprintf(path, sizeof(path), "%s.app%u.refused", opt_output.c_str(), opt_app);
   if (FILE *f = fopen(path, "w"))
   {
      fputs(msg, f);
      fclose(f);
   }
   /* _exit, not exit: a refusal comes from one vCPU's callback while the
      others run on, and exit() ran QEMU's and our exit handlers (at_exit
      closing every stream) and the static destructors under them -- QEMU
      then died of SIGSEGV in one refusal in four (riscv-user-mt's live run).
      Nothing those handlers do is wanted for a run being refused. */
   fflush(stderr);
   _exit(1);
}

/* A guest process created by fork (QEMU user mode runs a guest's fork,
   vfork or posix_spawn as a host fork, which runs this handler in the child).
   The plugin records one process: the child would inherit the parent's
   writers and write its instructions into the parent's streams, or into the
   parent's channel to Sniper, corrupting both.  Until children are given
   streams of their own, the child is refused with the reason, which fails the
   run (run-sniper reads the .refused marker); the parent runs on. */
static void refuse_forked_child(void)
{
   refuse("[SNIPER-QEMU] error: the guest created a process (fork, vfork or posix_spawn).\n"
          "  The QEMU front end records one process; a child would write into the\n"
          "  parent's streams.  Record each program on its own, or use the SDE\n"
          "  front end (--follow) for programs that create processes.\n");
}

const unsigned int MAX_VCPUS = 4096;
VCPU *vcpus[MAX_VCPUS];

VCPU *vcpu_get(unsigned int idx)
{
   if (idx >= MAX_VCPUS)
   {
      refuse("[SNIPER-QEMU] error: vcpu %u exceeds the %u this frontend tracks\n", idx, MAX_VCPUS);
   }
   if (!vcpus[idx])
   {
      vcpus[idx] = new VCPU();
      vcpus[idx]->vcpu_index = idx;
   }
   return vcpus[idx];
}

/* A callback's stage at an address: true to run it, false when it ran
 * before the wait this execution resumes from. */
inline bool stage_begin(VCPU *v, uint64_t vaddr, unsigned bit)
{
   if (!g_wait)
      return true;
   if (v->done_vaddr != vaddr)
   {
      v->done_vaddr = vaddr;
      v->done_mask = 0;
   }
   if (v->done_mask & bit)
      return false;
   v->done_mask |= bit;
   return true;
}

/* A block begins: after a wait, at the wait's instruction, with the stages
 * that ran still marked; otherwise a fresh visit. */
inline void block_begin(VCPU *v, uint64_t vaddr)
{
   if (!g_wait)
      return;
   if (v->resuming)
   {
      v->resuming = false;
      if (v->done_vaddr != vaddr)
      {
         v->done_vaddr = vaddr;
         v->done_mask = 0;
      }
      if (!v->wait_tb_level)
         v->done_mask |= S_TB_COUNT | S_TB_FLOW;   /* counted with the interrupted block */
   }
   else
   {
      v->done_vaddr = vaddr;
      v->done_mask = 0;
   }
}

void wait_body(unsigned int cpu_index, void *udata);

/* Leave the run loop to wait; the callback's own return address is what
 * lets QEMU restore the state at this instruction, so this is a macro used
 * in the callback itself (registered with QEMU_PLUGIN_CB_RW_REGS_PC).  Does
 * not return. */
#if QEMU_PLUGIN_VERSION >= 7
#define SNP_DEFER(v_, vaddr_, kind_, tb_level_) do {                                  \
      (v_)->wait_kind = (kind_); (v_)->wait_vaddr = (vaddr_);                            \
      (v_)->wait_tb_level = (tb_level_); (v_)->resuming = true;                          \
      qemu_plugin_vcpu_wait_at((vaddr_), wait_body, (v_), (uintptr_t)__builtin_return_address(0)); \
      __builtin_unreachable();                                                           \
   } while (0)
#else
#define SNP_DEFER(v_, vaddr_, kind_, tb_level_) abort()   /* g_wait is never set */
#endif

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
      refuse("[SNIPER-QEMU] cannot open %s\n", filename);
   }
   v->writer->setHandleAccessMemoryFunc(access_memory);
   v->writer->setOutOfProcess();            /* syscall arguments are guest addresses */
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
   if (!stage_begin(v, ii->vaddr, S_AME))
      return;                      /* executed before a wait this execution resumes from */
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

/* A mode the simulator asked for, in the answer to a count or a sync. */
void apply_mode(Sift::Mode mode)
{
   /* Outside a region of interest a live run only counts: an answer the
    * simulator gave before another thread's ROI end (or its start) arrived
    * can say detailed or memory, and would record what is not the ROI */
   if (opt_roi && !g_in_roi && (mode == Sift::ModeDetailed || mode == Sift::ModeMemory))
      mode = Sift::ModeIcount;
   switch (mode)
   {
      case Sift::ModeDetailed: g_mode.store(MODE_DETAILED); break;
      case Sift::ModeMemory:   g_mode.store(MODE_MEMORY); break;
      case Sift::ModeIcount:   g_mode.store(MODE_ICOUNT); break;
      default: break;          /* unknown: the simulator did not say; stop: handled by the writer */
   }
}

/* Report a vCPU's pending count.  Synchronous: the answer carries the mode
 * the simulator wants next, and waiting for it is the flow control between
 * measured regions. */
void ff_report(VCPU *v)
{
   if (!v->writer || v->closed || v->ff_pending == 0 || !opt_response)
      return;
   const Sift::Mode mode = v->writer->InstructionCount(v->ff_pending);
   v->ff_pending = 0;
   apply_mode(mode);
}

void insn_exec(unsigned int cpu_index, void *udata)
{
   const InsnInfo *ii = (const InsnInfo *)udata;
   const int mode = g_mode.load(std::memory_order_relaxed);
   VCPU *v = vcpu_get(cpu_index);
   if (!stage_begin(v, ii->vaddr, S_INSN))
      return;                      /* resumed after a wait: this instruction was recorded */
   /* The mode this instruction is recorded under, for its memory callbacks:
    * another thread's sampler may switch the mode between here and them,
    * and a record must be whole. */
   v->insn_mode = mode;
   if (mode == MODE_OFF)
      return;                      /* outside the ROI: nothing recorded, magic instructions excepted */
   if (!v->writer || v->closed)
      return;
   if (mode != MODE_DETAILED)
   {
      /* The instruction the detailed stretch left pending -- the switch came
       * in its own Sync(), before its memory callbacks -- goes out now, with
       * this instruction as its successor.  Left pending across the
       * fast-forward, it collected the first detailed instruction's address
       * after the switch back, and the simulator saw a memory operand on an
       * instruction that has none ("Expected memory 0 info, got: 1"). */
      if (v->have_pending)
         flush_pending(v, ii->vaddr);
      /* Between measured regions.  Memory-only: the previous branch's outcome
       * goes out now that the next instruction is known, accesses go out from
       * mem_access, each record carrying the count pending since the last;
       * counting: only the count, once per flow-control window. */
      if (mode == MODE_MEMORY && v->ff_prev_branch)
      {
         const bool taken = ii->vaddr != v->ff_prev_addr + v->ff_prev_size;
         const uint8_t carried = v->ff_pending > 250 ? 250 : (uint8_t)v->ff_pending;
         v->writer->CacheOnly(carried, taken ? Sift::CacheOnlyBranchTaken : Sift::CacheOnlyBranchNotTaken, v->ff_prev_addr, ii->vaddr);
         v->ff_pending -= carried;
      }
      v->icount++;
      v->ff_pending++;
      v->ff_prev_branch = mode == MODE_MEMORY && ii->is_branch;
      v->ff_prev_addr = ii->vaddr;
      v->ff_prev_size = ii->size;
      if (!g_wait && v->ff_pending >= (mode == MODE_MEMORY ? 200 : opt_flow))
         ff_report(v);              /* waiting: tb_begin's flow stage reports, outside the run loop */
      return;
   }

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
    * never blocks.  In user mode it is the blocking Sync() -- here, in the
    * callback, only when the QEMU cannot wait outside the run loop (then the
    * guest is single-threaded); otherwise from tb_begin's flow stage. */
   if (!g_wait && v->icount >= v->flow_next)
   {
      v->flow_next = v->icount + opt_flow;

      if (opt_response)
      {
         apply_mode(v->writer->Sync());   /* blocks until Sniper answers; the answer says which mode comes next */
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
   /* and, for a synchronisation point's call site, the stack pointer or the
    * return-address register */
   static const char *const retnames[3][2] = { {"rsp","esp"}, {"x30","lr"}, {"ra","x1"} };
   const unsigned a = g_arch == ARCH_X86 ? 0 : g_arch == ARCH_AARCH64 ? 1 : 2;
   GArray *regs = qemu_plugin_get_registers();
   for (guint i = 0; regs && i < regs->len; i++)
   {
      qemu_plugin_reg_descriptor *d = &g_array_index(regs, qemu_plugin_reg_descriptor, i);
      for (unsigned n = 0; n < 2; n++)
         if (!v->reg_ret_have && retnames[a][n] && strcmp(d->name, retnames[a][n]) == 0)
         {
            v->reg_ret = d->handle;
            v->reg_ret_have = true;
         }
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

/* The guest's vector register width, reported once to the simulator
   (SIM_CMD_VECTOR_BYTES), which refuses a configuration that models another:
   the RISC-V vector unit's vector_bytes and the SVE width matched the
   recording's VLEN only by convention, and a recording made with another
   -cpu vlen= was simulated at the wrong width without a word.  Read from the
   gdbstub's vlenb (RISC-V) or vg (AArch64, in 8-byte granules); a CPU without
   them reports nothing. */
std::atomic<bool> g_vector_width_reported(false);

void vector_width_exec(unsigned int cpu_index, void *udata)
{
   if (g_vector_width_reported.exchange(true))
      return;
   VCPU *v = vcpu_get(cpu_index);
   if (!v->writer || v->closed)
      return;
   const char *want = g_arch == ARCH_RISCV ? "vlenb" : "vg";
   uint64_t val = 0;
   bool found = false;
   GArray *regs = qemu_plugin_get_registers();
   for (guint i = 0; regs && i < regs->len && !found; i++)
   {
      qemu_plugin_reg_descriptor *d = &g_array_index(regs, qemu_plugin_reg_descriptor, i);
      if (strcmp(d->name, want) == 0)
      {
         GByteArray *b = g_byte_array_new();
         if (qemu_plugin_read_register(d->handle, b) && b->len)
         {
            memcpy(&val, b->data, std::min<guint>(b->len, sizeof(val)));
            found = true;
         }
         g_byte_array_free(b, TRUE);
      }
   }
   if (regs)
      g_array_free(regs, TRUE);
   if (!found || val == 0)
      return;
   const uint64_t bytes = g_arch == ARCH_RISCV ? val : val * 8;
   v->writer->Magic(SIM_CMD_VECTOR_BYTES, bytes, 0);
}

void magic_body(VCPU *v, const InsnInfo *ii, uint64_t cmd, uint64_t arg0, uint64_t arg1, unsigned int cpu_index);

void magic_exec(unsigned int cpu_index, void *udata)
{
   const InsnInfo *ii = (const InsnInfo *)udata;
   VCPU *v = vcpu_get(cpu_index);
   if (!v->writer || v->closed)
      return;
   if (!stage_begin(v, ii->vaddr, S_MAGIC))
      return;
   if (!v->mreg_found)
      find_magic_registers(v);

   const uint64_t cmd  = read_reg(v->mreg[1], v->mreg_have[1]);
   const uint64_t arg0 = read_reg(v->mreg[2], v->mreg_have[2]);
   const uint64_t arg1 = read_reg(v->mreg[3], v->mreg_have[3]);

   if (g_wait)
   {
      /* the question is asked outside the run loop, the answer written there */
      v->wait_a[0] = cmd; v->wait_a[1] = arg0; v->wait_a[2] = arg1; v->wait_ii = ii;
      SNP_DEFER(v, ii->vaddr, W_MAGIC, false);
   }
   magic_body(v, ii, cmd, arg0, arg1, cpu_index);
}

/* The magic instruction's work, in the callback or outside the run loop. */
void magic_body(VCPU *v, const InsnInfo *ii, uint64_t cmd, uint64_t arg0, uint64_t arg1, unsigned int cpu_index)
{
   if (!v->writer || v->closed)
      return;
   if (opt_roi && cmd == SIM_CMD_ROI_START && !g_in_roi)
   {
      g_in_roi = true;
      g_mode.store(MODE_DETAILED);
      if (opt_verbose)
         fprintf(stderr, "[SNIPER-QEMU] ROI begin on vcpu %u\n", cpu_index);
   }

   /* The magic instruction goes out first (when traced), then the record;
    * between measured regions the pending count goes first. */
   flush_pending(v, ii->vaddr + ii->size);
   if (g_mode.load() != MODE_DETAILED)
      ff_report(v);
   /* Recorded outside the ROI too (MODE_OFF): the trace keeps the markers
    * and queries around the ROI, as the other front ends' traces do -- a
    * SimInSimulator() after SimRoiEnd() is answered from the trace. */
   const uint64_t res = v->writer->Magic(cmd, arg0, arg1);

   if (opt_roi && cmd == SIM_CMD_ROI_START && pacsim_qemu::enabled())
      pacsim_qemu::roi_begin(cpu_index);
   if (opt_roi && cmd == SIM_CMD_ROI_END && g_in_roi)
   {
      if (pacsim_qemu::enabled())
         pacsim_qemu::roi_end(cpu_index);      /* measures the last region, writes the file */
      /* The guest runs on, recorded no more.  A live run stays live: the
       * instructions are counted to the simulator as before the ROI, so its
       * clock goes on (a guest timing itself or waiting on a deadline after
       * the ROI saw it frozen), every emulated call is still the simulator's
       * -- a thread that waited in its futex model is woken there, a join
       * waits there, a thread started after the ROI is announced -- and the
       * simulator ends with the guest.  A trace written to a file stops. */
      g_in_roi = false;
      g_mode.store(opt_response ? MODE_ICOUNT : MODE_OFF);
      if (opt_verbose)
         fprintf(stderr, "[SNIPER-QEMU] ROI end: %" PRIu64 " instructions recorded on vcpu %u\n", v->icount, cpu_index);
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

/* ----------------------------------------------------------- the sampler */

struct TBCount { uint32_t n; uint64_t vaddr; };

/* Would flow control report now?  Detailed: a Sync every opt_flow
 * instructions; otherwise the pending count, at the window the mode uses. */
inline bool flow_due(const VCPU *v, int mode)
{
   if (mode == MODE_DETAILED)
      return v->icount >= v->flow_next;
   if (mode == MODE_OFF)
      return false;
   return v->ff_pending >= (mode == MODE_MEMORY ? 200u : opt_flow);
}

void flow_report(VCPU *v)
{
   const int mode = g_mode.load();
   if (mode == MODE_DETAILED)
   {
      v->flow_next = v->icount + opt_flow;
      apply_mode(v->writer->Sync());   /* the answer says which mode comes next */
   }
   else if (mode != MODE_OFF)
      ff_report(v);
}

/* Every block's start: the sampler counts it (the block is a basic block
 * to it), and, when waiting, flow control reports from here -- a block
 * boundary, where leaving the run loop re-executes nothing. */
void tb_begin(unsigned int cpu_index, void *udata)
{
   const TBCount *tc = (const TBCount *)udata;
   VCPU *v = vcpu_get(cpu_index);
   block_begin(v, tc->vaddr);
   if (pacsim_qemu::enabled() && stage_begin(v, tc->vaddr, S_TB_COUNT))
   {
      if (pacsim_qemu::block(cpu_index, tc->n, tc->vaddr, !g_wait))
      {
         v->wait_n = tc->n;
         SNP_DEFER(v, tc->vaddr, W_TB_COUNT, true);
      }
   }
   if (g_wait && stage_begin(v, tc->vaddr, S_TB_FLOW))
   {
      const int mode = g_mode.load(std::memory_order_relaxed);
      if (v->writer && !v->closed && flow_due(v, mode))
         SNP_DEFER(v, tc->vaddr, W_FLOW, true);
   }
}

void backedge_exec(unsigned int cpu_index, void *udata)
{
   const InsnInfo *ii = (const InsnInfo *)udata;
   VCPU *v = vcpu_get(cpu_index);
   if (!stage_begin(v, ii->vaddr, S_BACKEDGE))
      return;
   v->hold_pending = true;          /* the branch has not run: its record waits for its outcome */
   const bool deferred = pacsim_qemu::back_edge(cpu_index, ii->vaddr, ii->is_branch, !g_wait);
   v->hold_pending = false;
   if (deferred)
   {
      v->wait_ii = ii;
      SNP_DEFER(v, ii->vaddr, W_BACKEDGE, false);
   }
}

/* Outside the run loop, on the vCPU's own host thread: what the callback
 * could not wait for.  Registers and memory are accessible here. */
/* The instructions a vCPU's thread has executed before an instruction
 * (tb_left: the block's count is added at its entry), or, without one, up
 * to its current block's end */
uint64_t thread_instructions(VCPU *v, unsigned int vcpu_index, uint32_t tb_left)
{
   if (!g_icount_sb)
      return 0;
   const uint64_t now = qemu_plugin_u64_get(g_icount, vcpu_index) - tb_left;
   return now > v->icount_base ? now - v->icount_base : 0;
}

/* ------------------------------------------------- the one-IPC model's clocks
 *
 * Where the simulator does not answer (a trace written to a file, a
 * functional run, a channel lost), the guest's time is the one-IPC model's
 * (sift/guest_time.h): per thread, its instructions at 1 GHz from its
 * parent's time at its start, plus the time it waited; timed waits end at
 * their deadline in that time.  User mode; slots are vCPU indices. */
struct ClockLock
{
   std::mutex m;
   void lock() { m.lock(); }
   void unlock() { m.unlock(); }
};
uint64_t clock_instructions(unsigned slot, void *)
{
   VCPU *v = vcpus[slot];
   return v ? thread_instructions(v, slot, 0) : 0;
}
typedef Sift::GuestClocks<ClockLock> Clocks;
Clocks *g_clocks = nullptr;

void clocks_start(VCPU *v, int parent)
{
   v->scratch = 0;                  /* a slot reused: its last thread's page went with it */
   v->restart_valid = false;
   v->clock_waiting = false;
   if (g_clocks)
      g_clocks->start(v->vcpu_index, parent);
}

void clocks_exit(VCPU *v)
{
   if (g_clocks && !in_system_mode)
      g_clocks->exit(v->vcpu_index, v->tid_ptr);
}

/* The one-IPC model's counters, where the simulator does not answer: one
 * instruction per cycle at Sift::ONE_IPC_HZ (1 GHz, assumed); the time, and
 * the cycles of a counter that counts while the thread waits (the TSC, the
 * cycle CSR, which count time), from the thread's clock */
void counter_one_ipc(VCPU *v, uint64_t instructions)
{
   v->ctr_time_ns = g_clocks ? g_clocks->at(v->vcpu_index, instructions) : Sift::one_ipc_ns(instructions);
   v->ctr_core_cycles = v->ctr_global_cycles = v->ctr_time_ns * Sift::ONE_IPC_HZ / Sift::NS_PER_S;
   v->ctr_instructions = instructions;
   v->ctr_valid = true;
}

/* A counter read's question to Sniper: the instruction itself goes out first
 * (as for a magic instruction), so the simulator answers at the time it has
 * reached having run it, and the answer waits for QEMU's counter callback
 * within the instruction.  In the callback, or outside the run loop. */
void counter_body(VCPU *v, const InsnInfo *ii, unsigned int cpu_index)
{
   if (v->writer && !v->closed)
   {
      flush_pending(v, ii->vaddr + ii->size);
      if (g_mode.load() != MODE_DETAILED)
         ff_report(v);
      Sift::EmuRequest req;
      Sift::EmuReply res;
      if (v->writer->Emulate(Sift::EmuTypeCounters, req, res))
      {
         v->ctr_time_ns = res.counters.time_ns;
         v->ctr_core_cycles = res.counters.core_cycles;
         v->ctr_global_cycles = res.counters.global_cycles;
         v->ctr_instructions = res.counters.instructions;
         v->ctr_valid = true;
         return;
      }
   }
   counter_one_ipc(v, thread_instructions(v, cpu_index, ii->tb_left));
}

/* Before a counter read executes: on a live channel, for as long as it is
 * open (as for futex calls: the guest's time stays the simulated time after
 * the ROI too), the simulator's answer; otherwise the one-IPC model's. */
void counter_exec(unsigned int cpu_index, void *udata)
{
   const InsnInfo *ii = (const InsnInfo *)udata;
   VCPU *v = vcpu_get(cpu_index);
   if (!stage_begin(v, ii->vaddr, S_COUNTER))
      return;
   v->ctr_valid = false;
   if (!v->writer || !opt_response || v->closed)
   {
      counter_one_ipc(v, thread_instructions(v, cpu_index, ii->tb_left));
      return;
   }
   if (g_wait)
   {
      v->wait_ii = ii;
      SNP_DEFER(v, ii->vaddr, W_COUNTER, false);
   }
   counter_body(v, ii, cpu_index);
}

/* QEMU's counter read, within the instruction: the answer counter_exec got
 * (the simulator's clocks, or the one-IPC model's), in the counter's units
 * -- a timer at its rate (AArch64's CNTFRQ as QEMU gives it, RISC-V's
 * timebase= option), cycles in the core's clock (the TSC in the global
 * clock, as the SIFT recorder's rdtsc), instructions the core ran.  A read
 * the translation did not recognise gets the one-IPC model up to its
 * block's end.  Never the host's clock. */
uint64_t counter_read(unsigned int vcpu_index, int counter, uint64_t freq_hz, uint64_t host_value, void *)
{
   VCPU *v = vcpu_get(vcpu_index);
   if (!v->ctr_valid)
      counter_one_ipc(v, thread_instructions(v, vcpu_index, 0));
   v->ctr_valid = false;
   switch (counter)
   {
      case SNP_COUNTER_TIME:
      {
         const uint64_t hz = freq_hz ? freq_hz : opt_timebase;
         return (uint64_t)((unsigned __int128)v->ctr_time_ns * hz / 1000000000u);
      }
      case SNP_COUNTER_CYCLES:
         return g_arch == ARCH_X86 ? v->ctr_global_cycles : v->ctr_core_cycles;
      case SNP_COUNTER_INSTRET:
         return v->ctr_instructions;
      default:
         return host_value;   /* a counter this plugin does not know */
   }
}

void wait_body(unsigned int cpu_index, void *udata)
{
   VCPU *v = (VCPU *)udata;
   const int kind = v->wait_kind;
   v->wait_kind = W_NONE;
   switch (kind)
   {
      case W_TB_COUNT:
         pacsim_qemu::block(cpu_index, v->wait_n, v->wait_vaddr, true);
         break;
      case W_FLOW:
         if (v->writer && !v->closed)
            flow_report(v);
         break;
      case W_MAGIC:
         magic_body(v, v->wait_ii, v->wait_a[0], v->wait_a[1], v->wait_a[2], cpu_index);
         break;
      case W_COUNTER:
         counter_body(v, v->wait_ii, cpu_index);
         break;
      case W_SYNC:
         v->hold_pending = true;    /* the instruction runs after this, on resume */
         pacsim_qemu::sync_point(cpu_index, v->wait_site, true);
         v->hold_pending = false;
         break;
      case W_BACKEDGE:
         v->hold_pending = true;
         pacsim_qemu::back_edge(cpu_index, v->wait_vaddr, v->wait_ii->is_branch, true);
         v->hold_pending = false;
         break;
      default:
         break;
   }
}

/* a synchronisation point's entry: keyed by its call site, the return
 * address -- on the stack for x86, in the link register otherwise */
void sync_exec(unsigned int cpu_index, void *udata)
{
   const InsnInfo *ii = (const InsnInfo *)udata;
   VCPU *v = vcpu_get(cpu_index);
   if (!stage_begin(v, ii->vaddr, S_SYNC))
      return;
   if (!v->mreg_found)
      find_magic_registers(v);
   uint64_t site = ii->vaddr;
   if (g_arch == ARCH_X86)
   {
      const uint64_t rsp = read_reg(v->reg_ret, v->reg_ret_have);
      GByteArray *buf = g_byte_array_new();
      if (rsp && qemu_plugin_read_memory_vaddr(rsp, buf, sizeof(uint64_t)) && buf->len == sizeof(uint64_t))
         memcpy(&site, buf->data, sizeof(site));
      g_byte_array_free(buf, TRUE);
   }
   else
      site = read_reg(v->reg_ret, v->reg_ret_have);
   v->hold_pending = true;          /* the entry instruction has not run: its record waits for its addresses */
   const bool deferred = pacsim_qemu::sync_point(cpu_index, site, !g_wait);
   v->hold_pending = false;
   if (deferred)
   {
      v->wait_site = site;
      SNP_DEFER(v, ii->vaddr, W_SYNC, false);
   }
}

/* An unconditional direct jump: x86 jmp with a relative displacement (an
 * indirect jmp has none); RISC-V jal with rd = x0, or c.j; AArch64 b.  A
 * loop's closing jump, when backward; the sampler decides whether it is a
 * loop worth cutting at.  Not a SIFT branch (is_branch: could have fallen
 * through). */
bool is_direct_jump(const uint8_t *bytes, size_t len)
{
   if (g_arch == ARCH_X86)
   {
      xed_decoded_inst_t xedd;
      xed_decoded_inst_zero(&xedd);
      xed_decoded_inst_set_mode(&xedd, XED_MACHINE_MODE_LONG_64, XED_ADDRESS_WIDTH_64b);
      if (xed_decode(&xedd, bytes, (unsigned int)len) != XED_ERROR_NONE)
         return false;
      return xed_decoded_inst_get_iclass(&xedd) == XED_ICLASS_JMP && xed_decoded_inst_get_branch_displacement_width(&xedd) > 0;
   }
   if (g_arch == ARCH_AARCH64)
   {
      if (len < 4) return false;
      const uint32_t w = (uint32_t)bytes[0] | ((uint32_t)bytes[1] << 8) | ((uint32_t)bytes[2] << 16) | ((uint32_t)bytes[3] << 24);
      return (w & 0xfc000000u) == 0x14000000u;                          /* b imm26 */
   }
   if (len >= 4 && (bytes[0] & 3) == 3)
   {
      const uint32_t w = (uint32_t)bytes[0] | ((uint32_t)bytes[1] << 8) | ((uint32_t)bytes[2] << 16) | ((uint32_t)bytes[3] << 24);
      return (w & 0x7f) == 0x6f && ((w >> 7) & 0x1f) == 0;          /* jal x0, imm */
   }
   if (len >= 2)
   {
      const uint16_t w = (uint16_t)bytes[0] | ((uint16_t)bytes[1] << 8);
      return (w & 3) == 1 && ((w >> 13) & 7) == 5;                   /* c.j */
   }
   return false;
}

/* The branch's target, for the back-edge test: x86 from XED's displacement
 * (conditional or jmp); RISC-V from the B-type or compressed CB-type
 * immediate, or the J-type / CJ-type one of a jump; AArch64 from the
 * imm19 (b.cond, cbz/cbnz), imm14 (tbz/tbnz) or imm26 (b) field. */
bool branch_target(const uint8_t *bytes, size_t len, uint64_t vaddr, uint8_t size, uint64_t &target)
{
   if (g_arch == ARCH_X86)
   {
      xed_decoded_inst_t xedd;
      xed_decoded_inst_zero(&xedd);
      xed_decoded_inst_set_mode(&xedd, XED_MACHINE_MODE_LONG_64, XED_ADDRESS_WIDTH_64b);
      if (xed_decode(&xedd, bytes, (unsigned int)len) != XED_ERROR_NONE)
         return false;
      target = vaddr + size + (int64_t)xed_decoded_inst_get_branch_displacement(&xedd);
      return true;
   }
   if (g_arch == ARCH_AARCH64)
   {
      if (len < 4) return false;
      const uint32_t w = (uint32_t)bytes[0] | ((uint32_t)bytes[1] << 8) | ((uint32_t)bytes[2] << 16) | ((uint32_t)bytes[3] << 24);
      int64_t imm;
      if ((w & 0xff000000u) == 0x54000000u || (w & 0x7e000000u) == 0x34000000u)      /* b.cond, cbz/cbnz: imm19 */
      { imm = (int64_t)((w >> 5) & 0x7ffff) << 2; if (imm & (1 << 20)) imm -= (1 << 21); }
      else if ((w & 0x7e000000u) == 0x36000000u)                                     /* tbz/tbnz: imm14 */
      { imm = (int64_t)((w >> 5) & 0x3fff) << 2; if (imm & (1 << 15)) imm -= (1 << 16); }
      else if ((w & 0xfc000000u) == 0x14000000u)                                     /* b: imm26 */
      { imm = (int64_t)(w & 0x3ffffff) << 2; if (imm & (1 << 27)) imm -= (1 << 28); }
      else
         return false;
      target = vaddr + imm;
      return true;
   }
   if (size == 4)
   {
      const uint32_t w = (uint32_t)bytes[0] | ((uint32_t)bytes[1] << 8) | ((uint32_t)bytes[2] << 16) | ((uint32_t)bytes[3] << 24);
      if ((w & 0x7f) == 0x63)                                         /* B-type */
      {
         int64_t imm = (int64_t)(((w >> 31) & 1) << 12 | ((w >> 7) & 1) << 11 | ((w >> 25) & 0x3f) << 5 | ((w >> 8) & 0xf) << 1);
         if (imm & (1 << 12)) imm -= (1 << 13);
         target = vaddr + imm;
         return true;
      }
      if ((w & 0x7f) == 0x6f)                                         /* jal: imm[20|10:1|11|19:12] */
      {
         int64_t imm = (int64_t)(((w >> 31) & 1) << 20 | ((w >> 12) & 0xff) << 12 | ((w >> 20) & 1) << 11 | ((w >> 21) & 0x3ff) << 1);
         if (imm & (1 << 20)) imm -= (1 << 21);
         target = vaddr + imm;
         return true;
      }
      return false;
   }
   if (size == 2)
   {
      const uint16_t w = (uint16_t)bytes[0] | ((uint16_t)bytes[1] << 8);
      if ((w & 3) == 1 && ((w >> 13) & 7) >= 6)                      /* c.beqz, c.bnez */
      {
         int64_t imm = (int64_t)(((w >> 12) & 1) << 8 | ((w >> 5) & 3) << 6 | ((w >> 2) & 1) << 5 | ((w >> 10) & 3) << 3 | ((w >> 3) & 3) << 1);
         if (imm & (1 << 8)) imm -= (1 << 9);
         target = vaddr + imm;
         return true;
      }
      if ((w & 3) == 1 && ((w >> 13) & 7) == 5)                      /* c.j: imm[11|4|9:8|10|6|7|3:1|5] */
      {
         int64_t imm = (int64_t)(((w >> 12) & 1) << 11 | ((w >> 11) & 1) << 4 | ((w >> 9) & 3) << 8 | ((w >> 8) & 1) << 10
                               | ((w >> 7) & 1) << 6 | ((w >> 6) & 1) << 7 | ((w >> 3) & 7) << 1 | ((w >> 2) & 1) << 5);
         if (imm & (1 << 11)) imm -= (1 << 12);
         target = vaddr + imm;
         return true;
      }
   }
   return false;
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
   const int mode = v->insn_mode;      /* the instruction's, not the moment's: another thread's sampler may have switched it since */
   /* An instruction still pending is a detailed record in the making,
    * whatever the mode now: the switch out of detailed comes in the
    * instruction's own Sync(), before its memory callbacks, and its
    * addresses belong to its record, not to a CacheOnly one -- sent there,
    * the record went out short of them and the simulator asserted on a
    * load with no address. */
   if (mode == MODE_MEMORY && !v->have_pending)
   {
      /* between measured regions, with the caches kept warm */
      if (!v->writer || v->closed)
         return;
      const uint8_t carried = v->ff_pending > 250 ? 250 : (uint8_t)v->ff_pending;
      v->writer->CacheOnly(carried, qemu_plugin_mem_is_store(info) ? Sift::CacheOnlyMemWrite : Sift::CacheOnlyMemRead, v->ff_prev_addr, vaddr);
      v->ff_pending -= carried;
      return;
   }
   if (!v->have_pending || mode == MODE_OFF)
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

/* The guest's system-call number -> the simulator's (x86-64 Linux) number
 * for the calls it models, or -1; *emulated says whether the simulator's
 * answer replaces the call.  x86-64 guests: the same numbers; RISC-V and
 * AArch64 guests: the asm-generic table.  The set and the split are the Pin
 * recorder's (sift/recorder/syscall_modeling.cc): futex, sched_yield, the
 * affinity calls, nanosleep and clock_nanosleep are emulated -- the kernel
 * does not run them and the guest gets the simulator's return value -- while
 * read, write, wait4 and exit_group are reported and run as usual.
 * clock_gettime and gettimeofday are emulated too, here only: the Pin
 * recorder replaces the libc routines instead (osemu/clock_replace), while
 * a guest under QEMU reaches them through QEMU's vDSO, which makes the system
 * call, and read the host's clock -- a simulated program timed itself in host
 * seconds.  The simulator answers them with simulated time.  The guest's
 * cycle and time counters (rdtime, rdcycle, CNTVCT) are instructions, not
 * calls, and still read the host's. */
static int modelled_syscall(int64_t num, bool *emulated)
{
   struct Entry { int generic, x86; bool emulated; };
   static const Entry table[] = {
      { 63, 0, false },     /* read */
      { 64, 1, false },     /* write */
      { 260, 61, false },   /* wait4 */
      { 94, 231, false },   /* exit_group */
      { 98, 202, true },    /* futex */
      { 124, 24, true },    /* sched_yield */
      { 122, 203, true },   /* sched_setaffinity */
      { 123, 204, true },   /* sched_getaffinity */
      { 101, 35, true },    /* nanosleep */
      { 115, 230, true },   /* clock_nanosleep */
      { 113, 228, true },   /* clock_gettime */
      { 169, 96, true },    /* gettimeofday */
   };
   for (size_t i = 0; i < sizeof(table) / sizeof(table[0]); i++)
   {
      if (g_arch == ARCH_X86 ? num == table[i].x86 : num == table[i].generic)
      {
         *emulated = table[i].emulated;
         return table[i].x86;
      }
   }
   return -1;
}

/* Does this thread's channel take requests now?  Throughout a live run,
 * before, in and after the ROI (counting outside it, as the recorder does);
 * a trace written to a file has no channel to ask (MODE_OFF). */
inline bool channel_open(const VCPU *v)
{
   return v->writer && opt_response && !v->closed && g_mode.load() != MODE_OFF;
}

/* A thread's tid goes with the first request on its channel, as the
 * recorder sends it (the simulator's model of the thread needs it, and the
 * channel is opened by the first request). */
void send_threadinfo(VCPU *v)
{
   if (v->threadinfo_pending)
   {
      v->threadinfo_pending = false;
      Sift::EmuRequest req;
      Sift::EmuReply res;
      req.setthreadinfo.tid = (uint64_t)syscall(SYS_gettid);   /* the guest's gettid is the host thread's */
      v->writer->Emulate(Sift::EmuTypeSetThreadInfo, req, res);
   }
}

/* Send a modelled system call to the simulator.  Returns false when the call
 * is not sent (not modelled, no response channel, outside the ROI); else the
 * simulator's return value is in *ret and *emulated says whether it replaces
 * the call. */
static bool forward_syscall(VCPU *v, int64_t num, const uint64_t args[6],
                            uint64_t *ret, bool *emulated)
{
   /* Emitting a syscall record asks Sniper a question, so it is only valid
    * when there is a response channel to answer on -- Sift::Writer::Syscall()
    * calls initResponse() unconditionally.  The Pin recorder gates the same
    * thing on KnobEmulateSyscalls.  Without a channel we are writing a trace
    * to a file and the syscall is simply not modelled. */
   /* Only the system calls the simulator models go out, by the numbers it
    * knows them by.  The simulator switches on its host's (x86-64) SYS_
    * constants; a RISC-V or AArch64 guest uses the asm-generic table, where
    * the same numbers mean other calls (unlinkat is x86-64's nanosleep, mlock
    * its clock_gettime, munlockall its exit_group), so sending them raw had
    * the simulator zero a guest pathname, abort, or end the run. */
   const int sys = modelled_syscall(num, emulated);
   if (sys < 0)
      return false;

   if (!channel_open(v))
      return false;

   send_threadinfo(v);

   /* Close out the instruction that issued the syscall first, and the count
    * pending between measured regions, so the call is placed after the
    * instructions that led to it -- with threads, the simulator orders the
    * calls of all of them by their cores' time. */
   flush_pending(v, 0);
   ff_report(v);

   *ret = v->writer->Syscall((uint16_t)sys, (const char *)args, 6 * sizeof(uint64_t));
   return true;
}

/* Threads, at the system calls that make and end them; the numbers are the
 * guest's (x86-64, or asm-generic for RISC-V and AArch64). */
const int64_t X86_SYS_CLONE = 56, X86_SYS_EXIT = 60, GENERIC_SYS_CLONE = 220, GENERIC_SYS_EXIT = 93;
const int64_t SYS_CLONE3_ANY = 435;           /* the same on both tables */
const uint64_t CLONE_THREAD_FLAG = 0x10000;

bool read_guest(uint64_t addr, void *dst, size_t size)
{
   GByteArray *buf = g_byte_array_new();
   const bool ok = qemu_plugin_read_memory_vaddr(addr, buf, size) && buf->len == size;
   if (ok)
      memcpy(dst, buf->data, size);
   g_byte_array_free(buf, TRUE);
   return ok;
}

void writer_close(VCPU *v);

/* The thread is ending (its exit call, the last thing it does).  A joiner
 * waits on its tid word, in Sniper's model of the futex as in the kernel's:
 * the word is cleared and the wake sent, as the recorder does at a thread's
 * end, and the stream ends.  Here, at the call's entry, rather than in the
 * vCPU's exit hook: QEMU runs that hook holding the lock its clone and exit
 * paths share, and a wait for Sniper under it stops every sibling that
 * reaches either -- one of which Sniper may be waiting for. */
void scratch_release(VCPU *v);
void thread_exit(VCPU *v)
{
#ifdef SNP_HAVE_SYSCALL_FILTER
   scratch_release(v);
#endif
   clocks_exit(v);
   if (!v->writer)
      return;
   if (channel_open(v))
   {
      flush_pending(v, 0);
      ff_report(v);
      if (v->tid_ptr)
      {
         const uint32_t zero = 0;
         GByteArray *buf = g_byte_array_new();
         g_byte_array_append(buf, (const guint8 *)&zero, sizeof(zero));
         qemu_plugin_write_memory_vaddr(v->tid_ptr, buf);
         g_byte_array_free(buf, TRUE);
         const uint64_t args[6] = { v->tid_ptr, FUTEX_WAKE, 1, 0, 0, 0 };
         v->writer->Syscall((uint16_t)SYS_futex, (const char *)args, sizeof(args));
      }
   }
   writer_close(v);
}

/* A clone that makes a thread: its child-tid word is queued now, since the
 * child may start before this call's return is seen; Sniper is told at the
 * return (syscall_ret), when the clone is known to have happened -- glibc's
 * clone3 attempt gets ENOSYS from QEMU before it falls back to clone, and a
 * thread announced for each attempt would leave Sniper waiting for one that
 * never starts.  Followed for every thread (the one-IPC model's clocks
 * start a child at its parent's time); Sniper is only told on a live
 * channel.  clone(flags, stack, parent_tid, child_tid, tls) on x86-64;
 * clone(flags, stack, parent_tid, tls, child_tid) on RISC-V and AArch64
 * (CLONE_BACKWARDS); clone3(struct clone_args *, size): flags at 0,
 * child_tid at 16.  A clone without CLONE_THREAD makes a process, which the
 * fork handler refuses (refuse_forked_child). */
void clone_entry(VCPU *v, bool clone3, const uint64_t args[6])
{
   uint64_t flags = args[0], child_tidptr = g_arch == ARCH_X86 ? args[3] : args[4];
   if (clone3)
   {
      uint64_t words[3] = {0, 0, 0};
      if (!read_guest(args[0], words, sizeof(words)))
         return;
      flags = words[0];
      child_tidptr = words[2];
   }
   if (!(flags & CLONE_THREAD_FLAG))
      return;
   std::lock_guard<std::mutex> lock(g_tid_lock);
   g_tid_ptrs.push_back(Clone{child_tidptr, (int)v->vcpu_index});
   v->clone_tidptr = child_tidptr;
   v->clone_pending = true;
}

/* Every system call's entry, on the calling thread, in QEMU's cpu loop --
 * outside the run loop, so a wait for Sniper's answer here holds up no
 * exclusive section, threads or not.  The thread calls first, then the
 * modelled ones.  A clone is followed on a live channel whose QEMU can wait
 * (g_wait), even after the ROI, when a thread started must still be counted
 * for the child waiting in vcpu_init; without the wait call the child is
 * refused there, and Sniper must not have been told of it (it would wait
 * for a stream that never opens). */
static bool syscall_entry(unsigned int vcpu_index, int64_t num, const uint64_t args[6],
                          uint64_t *ret, bool *emulated)
{
   VCPU *v = vcpu_get(vcpu_index);
   /* Every thread's start and end, traced or not, recording or not: the
    * one-IPC model's clocks follow them too */
   if (!in_system_mode && num == (g_arch == ARCH_X86 ? X86_SYS_EXIT : GENERIC_SYS_EXIT))
   {
      thread_exit(v);
      return false;
   }
   if (!in_system_mode &&
       (num == (g_arch == ARCH_X86 ? X86_SYS_CLONE : GENERIC_SYS_CLONE) || num == SYS_CLONE3_ANY))
   {
      clone_entry(v, num == SYS_CLONE3_ANY, args);
      return false;
   }
   if (!v->writer)
      return false;
   return forward_syscall(v, num, args, ret, emulated);
}

#ifdef SNP_HAVE_SYSCALL_FILTER
/* ------------------------------------------------------------ timed calls
 *
 * No time the guest gives reaches the host's kernel (sift/guest_time.h): a
 * timeout is the guest's, in the guest's time.  The calls the simulator
 * models (futex, the sleeps) it times itself, on a live channel.  Every
 * other timed wait the plugin runs itself (qemu_plugin_run_syscall(),
 * tools/qemu-11.1.1-plugin-syscall.patch) with timeouts of its own, in a
 * scratch page of the thread's in the guest, never with the guest's:
 *
 * - live, as probes that do not wait (a zero timeout), between which the
 *   simulator lets the thread sleep in simulated time (a nanosleep it
 *   models, of 1 us doubling to 1 ms), until the guest's deadline in
 *   simulated time;
 * - where the simulator does not answer, in slices of SLICE_NS of host time
 *   that only let the plugin look again, until the one-IPC model says the
 *   deadline has passed (GuestClocks: every running thread is past it, or
 *   no thread runs and it is the earliest).
 *
 * The calls are numbered as on x86-64 (guest_number maps the others). */

const int64_t E_INTR = 4, E_AGAIN = 11, E_FAULT = 14, E_INVAL = 22, E_NOSYS = 38, E_TIMEDOUT = 110;
const int64_t Q_ERESTARTSYS = 512;          /* QEMU's: a guest signal is pending, restart the call */
const uint64_t SLICE_NS = 1000000;          /* the host time between looks, without a simulator */
const uint64_t LIVE_SLEEP_MIN = 1000, LIVE_SLEEP_MAX = 1000000;  /* the simulated sleeps between live probes */
bool g_rv32 = false;                        /* riscv32: the time64 calls */

/* x86-64 number <-> the guest's: { generic, x86-64 } (-1: none) */
struct SysPair { int generic, x86; };
const SysPair SYS_PAIRS[] = {
   { 63, 0 }, { 64, 1 }, { 57, 3 }, { 67, 17 }, { 68, 18 }, { 65, 19 }, { 66, 20 },
   { 222, 9 }, { 215, 11 }, { 101, 35 }, { 103, 38 }, { 102, 36 }, { 203, 42 }, { 202, 43 },
   { 206, 44 }, { 207, 45 }, { 211, 46 }, { 212, 47 }, { 93, 60 }, { 260, 61 }, { 129, 62 },
   { 193, 65 }, { 189, 69 }, { 188, 70 }, { 32, 73 }, { 169, 96 }, { 165, 98 }, { 153, 100 },
   { 137, 128 }, { 138, 129 }, { 133, 130 }, { 130, 200 }, { 98, 202 }, { 4, 208 }, { 192, 220 },
   { 107, 222 }, { 110, 223 }, { 108, 224 }, { 109, 225 }, { 111, 226 }, { 113, 228 }, { 114, 229 },
   { 115, 230 }, { 131, 234 }, { 182, 242 }, { 183, 243 }, { 95, 247 }, { 72, 270 }, { 73, 271 },
   { 22, 281 }, { 85, 283 }, { 86, 286 }, { 87, 287 }, { 242, 288 }, { 240, 297 }, { 243, 299 },
   { 269, 307 }, { 292, 333 }, { 441, 441 }, { 449, 449 },
   { -1, 7 }, { -1, 23 }, { -1, 34 }, { -1, 37 }, { -1, 201 }, { -1, 232 },
};
/* riscv32 has only the time64 calls, with a 64-bit time_t: the same layouts */
const SysPair RV32_TIME64[] = {
   { 403, 228 }, { 406, 229 }, { 407, 230 }, { 409, 223 }, { 408, 224 }, { 411, 286 }, { 410, 287 },
   { 413, 270 }, { 414, 271 }, { 416, 333 }, { 417, 299 }, { 418, 242 }, { 419, 243 }, { 420, 220 },
   { 421, 128 }, { 422, 202 },
};

int x86_number(int64_t num)
{
   if (g_arch == ARCH_X86)
      return (int)num;
   if (g_rv32)
      for (const SysPair &p : RV32_TIME64)
         if (p.generic == num)
            return p.x86;
   for (const SysPair &p : SYS_PAIRS)
      if (p.generic == num)
         return p.x86;
   return -1;
}

int64_t guest_number(int x86)
{
   if (g_arch == ARCH_X86)
      return x86;
   if (g_rv32)
      for (const SysPair &p : RV32_TIME64)
         if (p.x86 == x86)
            return p.generic;
   for (const SysPair &p : SYS_PAIRS)
      if (p.x86 == x86)
         return p.generic;
   return -1;
}

bool run_guest(int64_t num, const uint64_t a[6], int64_t *ret)
{
   return SNP_HAVE_RUN_SYSCALL && num >= 0 && qemu_plugin_run_syscall(num, a[0], a[1], a[2], a[3], a[4], a[5], 0, 0, ret);
}

bool write_guest(uint64_t addr, const void *src, size_t size)
{
   GByteArray *b = g_byte_array_new();
   g_byte_array_append(b, (const guint8 *)src, size);
   const bool ok = qemu_plugin_write_memory_vaddr(addr, b);
   g_byte_array_free(b, TRUE);
   return ok;
}

/* The thread's scratch page in the guest: the plugin's own timeouts, at 0
 * (a probe's or a slice's) and 16 (a live sleep's).  Mapped on the first
 * timed wait, unmapped at the thread's exit. */
uint64_t scratch_page(VCPU *v)
{
   if (!v->scratch)
   {
      const uint64_t a[6] = { 0, 4096, 3 /* PROT_READ|PROT_WRITE */, 0x22 /* MAP_PRIVATE|MAP_ANONYMOUS */, (uint64_t)-1, 0 };
      int64_t r;
      if (run_guest(guest_number(9), a, &r) && r > 0 && (uint64_t)r < (uint64_t)-4096)
         v->scratch = (uint64_t)r;
   }
   return v->scratch;
}

void scratch_release(VCPU *v)
{
   if (v->scratch)
   {
      const uint64_t a[6] = { v->scratch, 4096, 0, 0, 0, 0 };
      int64_t r;
      run_guest(guest_number(11), a, &r);
      v->scratch = 0;
   }
}

bool put_timespec(uint64_t addr, uint64_t ns, bool timeval = false)
{
   const uint64_t w[2] = { ns / Sift::NS_PER_S, timeval ? ns % Sift::NS_PER_S / 1000 : ns % Sift::NS_PER_S };
   return write_guest(addr, w, sizeof(w));
}

/* The time a timed call ends at: now plus its timeout, or its deadline less
 * the clocks' zero (osemu/time_start).  now and cpu_now in the same time
 * (the simulator's, or the thread's in the one-IPC model).  false and *err
 * for a timeout the kernel would refuse. */
bool call_deadline(const Sift::TimedCall &tc, const uint64_t args[6], uint64_t now, uint64_t cpu_now,
                   uint64_t *deadline, int64_t *err)
{
   uint64_t v;
   if (tc.form == Sift::TO_MS)
      v = (uint64_t)(int64_t)(int32_t)args[tc.arg] * 1000000ull;
   else
   {
      int64_t w[2];
      if (!read_guest(args[tc.arg], w, sizeof(w)))
      {
         *err = -E_FAULT;
         return false;
      }
      const int64_t frac_max = tc.form == Sift::TO_TIMEVAL ? 1000000 : 1000000000;
      if (w[0] < 0 || w[1] < 0 || w[1] >= frac_max)
      {
         *err = -E_INVAL;
         return false;
      }
      const uint64_t sec = (uint64_t)w[0] > 9000000000ull ? 9000000000ull : (uint64_t)w[0];   /* ~285 years: no overflow */
      v = sec * Sift::NS_PER_S + (uint64_t)w[1] * (tc.form == Sift::TO_TIMEVAL ? 1000 : 1);
   }
   if (!tc.absolute)
      *deadline = now + v;
   else if (Sift::cputime_clock(tc.clock))
      *deadline = now + (v > cpu_now ? v - cpu_now : 0);
   else
   {
      const uint64_t zero = opt_time_start * Sift::NS_PER_S;
      *deadline = v > zero ? v - zero : 0;
   }
   return true;
}

/* The guest's arguments with the timeout replaced by the plugin's: ns of
 * waiting (0: none, a probe), in the scratch page */
bool own_timeout(const Sift::TimedCall &tc, uint64_t scratch, uint64_t a[6], uint64_t ns)
{
   switch (tc.form)
   {
      case Sift::TO_MS:
         a[tc.arg] = ns ? (ns + 999999) / 1000000 : 0;
         return true;
      case Sift::TO_TIMEVAL:
         a[tc.arg] = scratch;
         return put_timespec(scratch, ns, true);
      default:
         a[tc.arg] = scratch;
         if (tc.absolute)
         {
            /* a deadline on the host's clock, a slice ahead (zero: the epoch, past) */
            uint64_t at = 0;
            if (ns)
            {
               struct timespec ts;
               clock_gettime(tc.clock == 0 ? CLOCK_REALTIME : CLOCK_MONOTONIC, &ts);
               at = (uint64_t)ts.tv_sec * Sift::NS_PER_S + (uint64_t)ts.tv_nsec + ns;
            }
            return put_timespec(scratch, at);
         }
         return put_timespec(scratch, ns);
   }
}

/* What the call returns when its timeout passed with nothing to report */
int64_t timeout_result(int x86)
{
   switch (x86)
   {
      case Sift::X86_futex: case Sift::X86_futex_waitv:
      case Sift::X86_mq_timedsend: case Sift::X86_mq_timedreceive:
         return -E_TIMEDOUT;
      case Sift::X86_rt_sigtimedwait: case Sift::X86_semtimedop:
         return -E_AGAIN;
      default:
         return 0;   /* poll, select, epoll, io_getevents: nothing ready */
   }
}

/* poll, select and epoll end with EINTR when a signal's handler runs, as in
 * the kernel; the sleeps too.  The other waits are restarted after the
 * handler, waiting on to the same deadline. */
bool eintr_call(int x86, const Sift::TimedCall &tc)
{
   return tc.cls == Sift::TC_SLEEP || timeout_result(x86) == 0;
}

/* select's and pselect6's fd sets, which every call rewrites: the guest's, to
 * give each probe or slice */
struct FdSets
{
   std::vector<uint8_t> set[3];
   bool have[3] = {false, false, false};
   bool save(const uint64_t args[6])
   {
      const uint64_t n = args[0];
      if (n > 1u << 20)
         return false;
      const size_t bytes = (n + 63) / 64 * 8;
      for (int i = 0; i < 3; i++)
      {
         have[i] = args[1 + i] != 0 && bytes;
         if (have[i])
         {
            set[i].resize(bytes);
            if (!read_guest(args[1 + i], set[i].data(), bytes))
               return false;
         }
      }
      return true;
   }
   void restore(const uint64_t args[6]) const
   {
      for (int i = 0; i < 3; i++)
         if (have[i])
            write_guest(args[1 + i], set[i].data(), set[i].size());
   }
};

/* What the wait leaves behind, as the kernel does: the time left in
 * select's timeval and in pselect6's and ppoll's timespec (glibc's select
 * is pselect6, and reports it), a sleep's in its rem (relative sleeps, when
 * interrupted) */
void time_left(const Sift::TimedCall &tc, const uint64_t args[6], int x86, uint64_t deadline, uint64_t now, int64_t ret)
{
   const uint64_t left = deadline != Sift::NO_DEADLINE && deadline > now ? deadline - now : 0;
   if ((x86 == Sift::X86_select || x86 == Sift::X86_pselect6 || x86 == Sift::X86_ppoll)
       && args[tc.arg] && deadline != Sift::NO_DEADLINE)
      put_timespec(args[tc.arg], left, x86 == Sift::X86_select);
   if (tc.cls == Sift::TC_SLEEP && ret == -E_INTR && !tc.absolute && tc.rem_arg >= 0 && args[tc.rem_arg])
      put_timespec(args[tc.rem_arg], left);
}

/* A signal interrupted the wait: EINTR, or (QEMU_ERESTARTSYS) a restart that
 * waits on to the same deadline */
int64_t interrupted(VCPU *v, int64_t num, int x86, const Sift::TimedCall &tc, const uint64_t args[6],
                    uint64_t deadline, int64_t r)
{
   if (r != -Q_ERESTARTSYS)
      return r;
   if (eintr_call(x86, tc))
      return -E_INTR;
   v->restart_valid = true;
   v->restart_num = num;
   memcpy(v->restart_args, args, sizeof(v->restart_args));
   v->restart_deadline = deadline;
   return r;
}

/* A restarted wait's deadline (see interrupted) */
bool restarted(VCPU *v, int64_t num, const uint64_t args[6], uint64_t *deadline)
{
   const bool match = v->restart_valid && v->restart_num == num && !memcmp(v->restart_args, args, sizeof(v->restart_args));
   v->restart_valid = false;
   if (match)
      *deadline = v->restart_deadline;
   return match;
}

void no_run_syscall_warning()
{
   static std::atomic<bool> warned(false);
   if (!warned.exchange(true))
      fprintf(stderr, "[SNIPER-QEMU] warning: this QEMU has no qemu_plugin_run_syscall()"
                      " (tools/qemu-11.1.1-plugin-syscall.patch): the guest's timeouts run in host time\n");
}

/* recvmmsg's timeout is only looked at after a datagram has arrived: the
 * call is run without it, ending after the first datagram and whatever else
 * has arrived (MSG_WAITFORONE) -- as if the timeout passed then */
bool recvmmsg_untimed(const uint64_t args[6], int64_t *sysret)
{
   uint64_t a[6];
   memcpy(a, args, sizeof(a));
   a[3] |= 0x10000;   /* MSG_WAITFORONE */
   a[4] = 0;
   return run_guest(guest_number(Sift::X86_recvmmsg), a, sysret);
}

/* The one-IPC model's timed wait (no simulator to answer) */
bool timed_one_ipc(VCPU *v, int64_t num, int x86, const uint64_t args[6], const Sift::TimedCall &tc, int64_t *sysret)
{
   const unsigned slot = v->vcpu_index;
   const uint64_t word = x86 == Sift::X86_futex ? args[0] : 0;
   if (!SNP_HAVE_RUN_SYSCALL)
   {
      no_run_syscall_warning();
      return false;
   }
   if (x86 == Sift::X86_recvmmsg && args[4])
   {
      g_clocks->wait_begin(slot, Sift::NO_DEADLINE, 0);
      const bool ran = recvmmsg_untimed(args, sysret);
      g_clocks->wait_end(slot, Clocks::WOKEN);
      return ran;
   }
   if (Sift::timeout_infinite(tc, args))
   {
      if (tc.cls == Sift::TC_SLEEP)
      {
         *sysret = -E_FAULT;
         return true;
      }
      /* no time to keep: the call runs as the guest made it, counting as a wait */
      g_clocks->wait_begin(slot, Sift::NO_DEADLINE, word);
      v->clock_waiting = true;
      return false;
   }
   const uint64_t now = g_clocks->now(slot);
   uint64_t deadline;
   int64_t err = 0;
   if (!restarted(v, num, args, &deadline) &&
       !call_deadline(tc, args, now, g_clocks->process_cpu(), &deadline, &err))
   {
      *sysret = err;
      return true;
   }
   FdSets sets;
   const uint64_t scratch = scratch_page(v);
   if (!scratch || (tc.fdsets && !sets.save(args)))
   {
      *sysret = -E_FAULT;
      return true;
   }

   g_clocks->wait_begin(slot, deadline, word);
   Clocks::End how = Clocks::WOKEN;
   int64_t r = 0;
   bool idle_seen = false;
   uint64_t idle_epoch = 0;
   for (;;)
   {
      /* May the deadline pass?  When every running thread is past it, or
       * (twice in a row, nothing having changed) when no thread runs */
      bool expire = deadline <= now;
      if (!expire)
      {
         uint64_t epoch;
         const int m = g_clocks->may_expire(slot, &epoch);
         expire = m == 1 || (m == 2 && idle_seen && epoch == idle_epoch);
         idle_seen = m == 2;
         idle_epoch = epoch;
      }
      uint64_t a[6];
      memcpy(a, args, sizeof(a));
      if (tc.cls == Sift::TC_SLEEP)
      {
         if (expire)
         {
            how = Clocks::TIMED_OUT;
            r = 0;
            break;
         }
         const uint64_t s[6] = { scratch, 0, 0, 0, 0, 0 };
         if (!put_timespec(scratch, SLICE_NS) || !run_guest(guest_number(Sift::X86_nanosleep), s, &r))
            r = -E_FAULT;
         if (r == 0)
            continue;
         how = Clocks::INTERRUPTED;
         break;
      }
      /* the last look, once the deadline has passed, waits not at all */
      sets.restore(args);
      if (!own_timeout(tc, scratch, a, expire ? 0 : SLICE_NS) || !run_guest(num, a, &r))
      {
         r = -E_FAULT;
         break;
      }
      if (r == timeout_result(x86))
      {
         if (expire)
         {
            how = Clocks::TIMED_OUT;
            break;
         }
         continue;
      }
      how = r == -E_INTR || r == -Q_ERESTARTSYS ? Clocks::INTERRUPTED : Clocks::WOKEN;
      break;
   }
   g_clocks->wait_end(slot, how);
   if (how == Clocks::INTERRUPTED)
      r = interrupted(v, num, x86, tc, args, deadline, r);
   time_left(tc, args, x86, deadline, g_clocks->now(slot), r);
   *sysret = r;
   return true;
}

/* The simulator's time (without the clocks' zero), after the instructions
 * that led here */
void send_threadinfo(VCPU *v);
bool sim_time(VCPU *v, uint64_t *ns)
{
   if (!channel_open(v))
      return false;
   send_threadinfo(v);
   flush_pending(v, 0);
   ff_report(v);
   Sift::EmuRequest req;
   Sift::EmuReply res;
   if (!v->writer->Emulate(Sift::EmuTypeCounters, req, res))
      return false;
   *ns = res.counters.time_ns;
   return true;
}

/* A live timed wait the simulator does not model: probes that do not wait,
 * and between them a sleep in simulated time */
bool timed_live(VCPU *v, int64_t num, int x86, const uint64_t args[6], const Sift::TimedCall &tc, int64_t *sysret)
{
   if (tc.cls == Sift::TC_SLEEP || x86 == Sift::X86_futex)
      return false;                 /* the simulator's (forward_syscall) */
   if (x86 == Sift::X86_futex_waitv)
   {
      /* a futex wait outside the simulator's model of the futexes, which no
       * wake it models would reach */
      *sysret = -E_NOSYS;
      return true;
   }
   if (!SNP_HAVE_RUN_SYSCALL)
   {
      no_run_syscall_warning();
      return false;
   }
   if (x86 == Sift::X86_recvmmsg)
      return args[4] && recvmmsg_untimed(args, sysret);
   uint64_t now;
   if (!sim_time(v, &now))
      return false;
   uint64_t deadline = Sift::NO_DEADLINE;
   int64_t err = 0;
   if (!Sift::timeout_infinite(tc, args) && !restarted(v, num, args, &deadline) &&
       !call_deadline(tc, args, now, now, &deadline, &err))
   {
      *sysret = err;
      return true;
   }
   FdSets sets;
   const uint64_t scratch = scratch_page(v);
   if (!scratch || (tc.fdsets && !sets.save(args)))
   {
      *sysret = -E_FAULT;
      return true;
   }
   uint64_t sleep = LIVE_SLEEP_MIN;
   int64_t r = 0;
   for (;;)
   {
      uint64_t a[6];
      memcpy(a, args, sizeof(a));
      sets.restore(args);
      if (!own_timeout(tc, scratch, a, 0) || !run_guest(num, a, &r))
      {
         r = -E_FAULT;
         break;
      }
      if (r != timeout_result(x86) || (deadline != Sift::NO_DEADLINE && now >= deadline))
         break;
      uint64_t s = sleep;
      if (deadline != Sift::NO_DEADLINE && deadline - now < s)
         s = deadline - now;
      const uint64_t sa[6] = { scratch + 16, 0, 0, 0, 0, 0 };
      uint64_t sret;
      bool emulated = false;
      if (!put_timespec(scratch + 16, s) || !forward_syscall(v, guest_number(Sift::X86_nanosleep), sa, &sret, &emulated)
          || !emulated || !sim_time(v, &now))
         break;                     /* the channel went: what the probe found */
      sleep = std::min(sleep * 2, LIVE_SLEEP_MAX);
   }
   if (r == -E_INTR || r == -Q_ERESTARTSYS)
      r = interrupted(v, num, x86, tc, args, deadline, r);
   time_left(tc, args, x86, deadline, now, r);
   *sysret = r;
   return true;
}

/* Where the simulator does not answer, the calls that can block (the
 * thread counts as waiting in the one-IPC model while in them) and those
 * that wake others (a waiter woken takes at least the waker's time) */
bool can_block(int x86, const uint64_t args[6])
{
   switch (x86)
   {
      case 0: case 17: case 19: case 45: case 47: case 43: case 288: case 42: case 61: case 247:
      case 70: case 69: case 65: case 34: case 130: case 73:
      case 1: case 20: case 44: case 46: case 307:
         return true;
      case Sift::X86_futex:
      {
         bool absolute;
         int clock;
         return Sift::futex_timed_op(args[1], &absolute, &clock);   /* the waits; the timed ones do not reach here */
      }
      default:
         return false;
   }
}

void note_wakes(VCPU *v, int x86, const uint64_t args[6])
{
   switch (x86)
   {
      case 1: case 18: case 20: case 44: case 46: case 307: case 3:      /* write, send, close */
      case 62: case 200: case 234: case 129: case 297:                   /* signals */
         g_clocks->woke(v->vcpu_index, 0);
         break;
      case Sift::X86_futex:
         switch (args[1] & 127)
         {
            case 1: case 7: case 10:           /* WAKE, UNLOCK_PI, WAKE_BITSET */
               g_clocks->woke(v->vcpu_index, args[0]);
               break;
            case 3: case 4: case 5: case 12:   /* REQUEUE, CMP_REQUEUE, WAKE_OP, CMP_REQUEUE_PI */
               g_clocks->woke(v->vcpu_index, args[0]);
               g_clocks->woke(v->vcpu_index, args[4]);
               break;
         }
         break;
   }
}

/* A call the simulator did not answer: a timed wait, kept in the guest's
 * time (live: the simulator's; otherwise the one-IPC model's); otherwise the
 * one-IPC model's bookkeeping, and QEMU runs the call */
bool timed_call(VCPU *v, int64_t num, const uint64_t args[6], int64_t *sysret)
{
   const int x86 = x86_number(num);
   if (x86 < 0)
      return false;
   Sift::TimedCall tc;
   const bool timed = Sift::timed_call(x86, args, &tc);
   if (channel_open(v))
      return timed && timed_live(v, num, x86, args, tc, sysret);
   if (!g_clocks)
      return false;
   note_wakes(v, x86, args);
   if (timed)
      return timed_one_ipc(v, num, x86, args, tc, sysret);
   if (can_block(x86, args))
   {
      g_clocks->wait_begin(v->vcpu_index, Sift::NO_DEADLINE, x86 == Sift::X86_futex ? args[0] : 0);
      v->clock_waiting = true;
   }
   return false;
}

/* The emulated calls are skipped and answered with the simulator's value, as
 * the Pin recorder does (it turns them into getpid and sets the result).
 * Before this QEMU ran them too and the answer was dropped: a nanosleep slept
 * in host time on top of the simulated time, sched_getaffinity reported the
 * host's CPUs, sched_yield and futex took no account of the simulated
 * scheduler -- and with threads, a futex wait slept in the host kernel while
 * the simulator held the thread that would wake it. */
/* clock_gettime and gettimeofday the simulator does not answer (a trace
 * written to a file): the one-IPC model's time, from the same time zero as
 * the simulator's, never the host's.  The CPU-time clocks (the process's, the
 * thread's, the dynamic ones: negative ids) count from zero, as the
 * simulator's do. */
static bool time_one_ipc(VCPU *v, int x86, const uint64_t args[6], int64_t *sysret)
{
   const bool tod = x86 == 96, secs = x86 == 201;
   if (!tod && !secs && x86 != 228)
      return false;
   const int32_t clock = tod || secs ? 0 : (int32_t)args[0];
   /* the thread's CPU-time clock: CLOCK_THREAD_CPUTIME_ID, a dynamic
    * thread clock (CPUCLOCK_PERTHREAD); the process's: the rest */
   const bool thread_cpu = clock == 3 || (clock < 0 && (clock & 4));
   const bool process_cpu = !thread_cpu && Sift::cputime_clock(clock);
   const uint64_t ns = thread_cpu ? Sift::one_ipc_ns(thread_instructions(v, v->vcpu_index, 0))
                     : process_cpu ? (g_clocks ? g_clocks->process_cpu() : 0)
                     : opt_time_start * Sift::NS_PER_S + (g_clocks ? g_clocks->now(v->vcpu_index) : 0);
   if (secs)
   {
      /* time(2): seconds, also stored at its argument */
      const uint64_t s = ns / Sift::NS_PER_S;
      *sysret = (int64_t)s;
      if (args[0] && !write_guest(args[0], &s, sizeof(s)))
         *sysret = -E_FAULT;
      return true;
   }
   const uint64_t out = tod ? args[0] : args[1];
   if (out)
   {
      const uint64_t words[2] = { ns / 1000000000ull, tod ? ns / 1000 % 1000000 : ns % 1000000000ull };
      GByteArray *b = g_byte_array_new();
      g_byte_array_append(b, (const guint8 *)words, sizeof(words));
      const bool ok = qemu_plugin_write_memory_vaddr(out, b);
      g_byte_array_free(b, TRUE);
      if (!ok)
      {
         *sysret = -14;   /* EFAULT, as the kernel answers an unmapped pointer */
         return true;
      }
   }
   *sysret = 0;
   return true;
}

bool syscall_filter(SNP_SYSCALL_FILTER_ARGS)
{
   const uint64_t args[6] = { a1, a2, a3, a4, a5, a6 };
   uint64_t ret = 0;
   bool emulated = false;
   if (syscall_entry(vcpu_index, num, args, &ret, &emulated) && emulated)
   {
      *sysret = (int64_t)ret;
      return true;
   }
   if (in_system_mode)
      return false;
   VCPU *v = vcpu_get(vcpu_index);
   const int x86 = x86_number(num);
   /* the time calls: the simulator's (live), else the one-IPC model's;
    * time(2), which the simulator does not model, from its time */
   if (x86 == 201 && channel_open(v))
   {
      uint64_t now;
      if (sim_time(v, &now))
      {
         const uint64_t s = opt_time_start + now / Sift::NS_PER_S;
         *sysret = (int64_t)s;
         if (args[0] && !write_guest(args[0], &s, sizeof(s)))
            *sysret = -E_FAULT;
         return true;
      }
   }
   if (x86 >= 0 && !channel_open(v) && time_one_ipc(v, x86, args, sysret))
      return true;
   return timed_call(v, num, args, sysret);
}
#else
/* Plugin API before v7: no filter, so the call is reported and QEMU runs it;
 * the simulator's answer cannot reach the guest. */
void syscall_enter(SNP_SYSCALL_ARGS)
{
   const uint64_t args[6] = { a1, a2, a3, a4, a5, a6 };
   uint64_t ret;
   bool emulated;
   syscall_entry(vcpu_index, num, args, &ret, &emulated);
}
#endif

/* A clone returned: a child tid means a thread started, and Sniper is told
 * so (NewThread: it opens the next stream); an error means the queued tid
 * word was never taken.  The announcement is counted either way, for the
 * child waiting in vcpu_init. */
void syscall_ret(SNP_SYSCALL_RET_ARGS)
{
   VCPU *v = vcpu_get(vcpu_index);
   if (v->clock_waiting)
   {
      /* a call the thread waited in (the one-IPC model): it returned */
      v->clock_waiting = false;
      if (g_clocks)
         g_clocks->wait_end(vcpu_index, ret == -4 || ret == -512 ? Clocks::INTERRUPTED : Clocks::WOKEN);
   }
   if (!v->clone_pending)
      return;
   v->clone_pending = false;
   if (ret > 0)
   {
      bool sent = false;
      if (channel_open(v))
      {
         /* The count first, as the recorder sends it before any call: a
          * thread parked by a blocking call (read, wait4) is on no core in
          * Sniper's model until its next instructions arrive, and the
          * creation is stamped with its core's time. */
         flush_pending(v, 0);
         ff_report(v);
         v->writer->NewThread();
         sent = true;
      }
      {
         std::lock_guard<std::mutex> lock(g_tid_lock);
         g_announce_sent.push_back(sent);
      }
      g_announce_cv.notify_all();
      return;
   }
   std::lock_guard<std::mutex> lock(g_tid_lock);
   for (size_t i = g_tid_ptrs.size(); i-- > 0; )
      if (g_tid_ptrs[i].tid_ptr == v->clone_tidptr && g_tid_ptrs[i].parent == (int)v->vcpu_index)
      {
         g_tid_ptrs.erase(g_tid_ptrs.begin() + i);
         break;
      }
}

void vcpu_init(SNP_VCPU_ARGS)
{
   /* In user mode a vCPU index is reused by a thread started after another
    * ended; its slot is then a fresh one.  Under the close lock: at_exit
    * walks the slots under it. */
   if (!in_system_mode && cpu_index < MAX_VCPUS && vcpus[cpu_index] && vcpus[cpu_index]->closed)
   {
      std::lock_guard<std::mutex> lock(g_close_lock);
      VCPU *old = vcpus[cpu_index];
      vcpus[cpu_index] = nullptr;
      delete old->writer;
      delete old;
   }
   VCPU *v = vcpu_get(cpu_index);
   if (g_icount_sb)   /* the thread's instructions count from here (an index is reused) */
      v->icount_base = qemu_plugin_u64_get(g_icount, cpu_index);
   if (g_arch == ARCH_RISCV)
      ame_register_state(v, cpu_index);
   if (pacsim_qemu::enabled())
      pacsim_qemu::thread_start(cpu_index);
   /* The thread's clone (the first thread has none): its tid word, and its
    * parent, whose time its clock starts at */
   if (!in_system_mode)
   {
      int parent = -1;
      if (g_main_started.exchange(true))
      {
         std::lock_guard<std::mutex> lock(g_tid_lock);
         if (!g_tid_ptrs.empty())
         {
            v->tid_ptr = g_tid_ptrs.front().tid_ptr;
            parent = g_tid_ptrs.front().parent;
            g_tid_ptrs.erase(g_tid_ptrs.begin());
         }
      }
      clocks_start(v, parent);
   }

   /* Checked before anything is opened, so a user who asks for this gets the
    * explanation rather than whatever the SIFT writer says about its response
    * channel.
    *
    * A second guest thread in user mode deadlocks a QEMU that cannot wait
    * outside its run loop: a vCPU blocked in Sync() still counts as running,
    * so anything needing an exclusive section waits for it.  Refuse it
    * rather than hang. */
   if (!in_system_mode && opt_response && !g_wait && cpu_index > 0)
   {
      refuse("[SNIPER-QEMU] error: the guest started a second thread.\n"
             "  This QEMU has no qemu_plugin_vcpu_wait_at() (tools/qemu-11.1.1-plugin-wait.patch),\n"
             "  so user-mode flow control can only be used with single-threaded\n"
             "  guests -- a vCPU blocked in a plugin callback stalls any other\n"
             "  vCPU that needs an exclusive section, and deadlocks when the\n"
             "  timing model holds it.  Build the patched QEMU (deps/sniper-deps\n"
             "  applies it), use full-system mode for multi-core, or the\n"
             "  Pin/DynamoRIO frontends.\n");
   }

   if (!v->writer)
      if (!opt_trace)
      return;                      /* a functional run: no trace file */
   if (!in_system_mode)
   {
      v->thread_num = g_num_threads.fetch_add(1);
      std::unique_lock<std::mutex> lock(g_tid_lock);
      if (v->thread_num > 0 && opt_response)
      {
         /* This hook runs in the cpu loop, outside cpu_exec(): waiting here
          * holds up no exclusive section.  A thread whose parent's clone
          * returned after the ROI was not announced: nothing to record, no
          * channel to open. */
         const unsigned n = v->thread_num;
         g_announce_cv.wait(lock, [n] { return g_announce_sent.size() >= n; });
         if (!g_announce_sent[n - 1])
            return;
         v->threadinfo_pending = true;
      }
   }
   else
      v->thread_num = cpu_index;
   writer_open(v, v->thread_num);

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
void writer_close_locked(VCPU *v)
{
   if (!v->writer || v->closed)
      return;
   flush_pending(v, 0);
   v->writer->End();
   v->closed = true;
}

void writer_close(VCPU *v)
{
   std::lock_guard<std::mutex> lock(g_close_lock);
   writer_close_locked(v);
}

/* In user mode normally closed already, by thread_exit(); this runs under
 * QEMU's clone lock (see there) and must not wait for Sniper. */
void vcpu_exit(SNP_VCPU_ARGS)
{
   writer_close(vcpu_get(cpu_index));
}

void tb_trans(SNP_TB_TRANS_ARGS)
{
   const size_t n = qemu_plugin_tb_n_insns(tb);
   TBInfo *ti = new TBInfo();
   ti->mode_mask = MODE_16 | MODE_32 | MODE_64;
   if (g_icount_sb)   /* every mode, traced or not: the one-IPC model's clock */
      qemu_plugin_register_vcpu_tb_exec_inline_per_vcpu(tb, QEMU_PLUGIN_INLINE_ADD_U64, g_icount, n);

   for (size_t i = 0; i < n; i++)
   {
      struct qemu_plugin_insn *insn = qemu_plugin_tb_get_insn(tb, i);

      /* One record per static instruction, reused by every execution. */
      InsnInfo *ii = new InsnInfo();
      ii->vaddr = qemu_plugin_insn_vaddr(insn);
      ii->tb_left = (uint32_t)(n - i);
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
      ii->counter = got && is_counter_read(bytes, got);
      ii->is_jump = got ? is_direct_jump(bytes, got) : false;

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
      if (i == 0 && (g_arch == ARCH_RISCV || g_arch == ARCH_AARCH64) && !g_vector_width_reported.load())
         qemu_plugin_register_vcpu_insn_exec_cb(insn, vector_width_exec, QEMU_PLUGIN_CB_R_REGS, ii);
      /* A callback that may leave the run loop to wait (g_wait) needs the
       * registers written back and the PC settable (RW_REGS_PC); the
       * tracer's never waits. */
      if (ii->magic)   /* after the tracer's callback: the instruction is pending */
         qemu_plugin_register_vcpu_insn_exec_cb(insn, magic_exec,
                                                g_wait ? QEMU_PLUGIN_CB_RW_REGS_PC : QEMU_PLUGIN_CB_RW_REGS, ii);
      if (ii->counter && g_counters)   /* after the tracer's callback: the instruction is pending */
         qemu_plugin_register_vcpu_insn_exec_cb(insn, counter_exec,
                                                g_wait ? QEMU_PLUGIN_CB_RW_REGS_PC : QEMU_PLUGIN_CB_NO_REGS, ii);
      if (pacsim_qemu::enabled())
      {
         if (i == 0 && pacsim_qemu::is_sync_entry(ii->vaddr))
            qemu_plugin_register_vcpu_insn_exec_cb(insn, sync_exec, g_wait ? QEMU_PLUGIN_CB_RW_REGS_PC : QEMU_PLUGIN_CB_R_REGS, ii);
         if (i + 1 == n && (ii->is_branch || ii->is_jump))   /* a backward branch or jump: a loop's back-edge, for the sampler to judge */
         {
            uint64_t target = 0;
            if (got && branch_target(bytes, got, ii->vaddr, ii->size, target) && target < ii->vaddr)
               qemu_plugin_register_vcpu_insn_exec_cb(insn, backedge_exec, g_wait ? QEMU_PLUGIN_CB_RW_REGS_PC : QEMU_PLUGIN_CB_NO_REGS, ii);
         }
      }
      if (ii->ame)     /* after the tracer's callback: the record is pending when the model runs */
         qemu_plugin_register_vcpu_insn_exec_cb(insn, ame_exec, ii->claimed ? QEMU_PLUGIN_CB_RW_REGS : QEMU_PLUGIN_CB_RW_REGS_PC, ii);
      qemu_plugin_register_vcpu_mem_cb(insn, mem_access,
                                       QEMU_PLUGIN_CB_NO_REGS,
                                       QEMU_PLUGIN_MEM_RW, nullptr);
   }

   if ((pacsim_qemu::enabled() || g_wait) && opt_trace)
   {
      TBCount *tc = new TBCount();  /* the sampler counts every block, in every mode; waiting: flow control from here */
      tc->n = (uint32_t)n;
      tc->vaddr = qemu_plugin_insn_vaddr(qemu_plugin_tb_get_insn(tb, 0));
      qemu_plugin_register_vcpu_tb_exec_cb(tb, tb_begin, g_wait ? QEMU_PLUGIN_CB_RW_REGS_PC : QEMU_PLUGIN_CB_NO_REGS, tc);
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
   if (pacsim_qemu::enabled())
      pacsim_qemu::at_exit();
   uint64_t total = 0;
   for (size_t i = 0; i < MAX_VCPUS; i++)
   {
      VCPU *v = vcpus[i];
      std::lock_guard<std::mutex> lock(g_close_lock);
      if (!v || !v->writer)
         continue;
      writer_close_locked(v);
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
   {
      g_arch = ARCH_RISCV;
      g_rv32 = !strcmp(info->target_name, "riscv32");
   }
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
      else if (key == "timebase") opt_timebase = strtoull(val.c_str(), nullptr, 0);   /* RISC-V time CSR rate, Hz */
      else if (key == "time_start") opt_time_start = strtoull(val.c_str(), nullptr, 0);   /* the guest's time zero, seconds since the epoch */
      else if (key == "roi")      opt_roi = val != "0" && val != "off" && val != "false";
      else if (pacsim_qemu::option(key, val)) {}                 /* pacsim, pacsim-* */
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
   /* Waiting outside the run loop: user mode on a live channel, when the
    * QEMU has the call (see "waiting"). */
   g_wait = SNP_HAVE_WAIT && opt_response && !in_system_mode && opt_trace;
   if (opt_verbose && opt_response && !in_system_mode)
      fprintf(stderr, "[SNIPER-QEMU] %s\n", g_wait ? "waits for Sniper outside the run loop: multi-threaded guests run"
                                                    : "this QEMU cannot wait outside the run loop: single-threaded guests only");
   /* Before the ROI: counting on a live channel (the simulator fast-forwards
    * on the counts, as under the Pin recorder), nothing to a file. */
   g_mode.store(!opt_roi ? MODE_DETAILED : opt_response ? MODE_ICOUNT : MODE_OFF);
   g_in_roi = !opt_roi;

   if (pacsim_qemu::enabled())
   {
      if (!opt_response || !opt_roi || !opt_trace)
      {
         fprintf(stderr, "[SNIPER-QEMU] error: 'pacsim' needs 'response' and 'roi': the sampler asks Sniper questions and samples the ROI.\n");
         return -1;
      }
      pacsim_qemu::Hooks h;
      h.in_roi = []() { return g_in_roi; };
      h.channel_alive = [](unsigned cpu) { VCPU *v = vcpus[cpu]; return v && v->writer && !v->closed; };
      /* A request from a sampler event at an instruction that has not run
       * yet (a synchronisation entry, a back-edge: their callbacks precede
       * the instruction) leaves that instruction's record pending -- it
       * goes out after the next instruction begins, whole, with its
       * addresses and its branch's outcome.  The request precedes it in
       * the stream by that one instruction. */
      h.magic = [](unsigned cpu, uint64_t cmd, uint64_t arg) -> uint64_t {
         VCPU *v = vcpus[cpu];
         if (!v || !v->writer || v->closed) return 0;
         if (!v->hold_pending) flush_pending(v, 0);
         if (g_mode.load() != MODE_DETAILED) ff_report(v);
         return v->writer->Magic(cmd, arg, 0);
      };
      h.set_mode = [](int mode) { g_mode.store(mode); };
      h.flush_counts = [](unsigned cpu) { VCPU *v = vcpus[cpu]; if (v) { if (!v->hold_pending) flush_pending(v, 0); ff_report(v); } };
      pacsim_qemu::activate(h, MAX_VCPUS, opt_verbose);
      pacsim_qemu::announce();
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
   /* Instructions per vCPU, for the one-IPC model */
   g_icount_sb = qemu_plugin_scoreboard_new(sizeof(uint64_t));
   g_icount = qemu_plugin_scoreboard_u64(g_icount_sb);
   /* The guest's counters answered with Sniper's clocks, or where Sniper is
    * not asked (a trace written to a file) the one-IPC model's: in user
    * mode, with the QEMU call */
   if (SNP_HAVE_COUNTERS && !in_system_mode)
   {
      qemu_plugin_register_counter_cb(id, SNP_COUNTER_CB(counter_read), nullptr);
      g_counters = true;
   }
#ifdef SNP_HAVE_SYSCALL_FILTER
   SNP_REG_SYSCALL_FILTER(id, syscall_filter);
#else
   SNP_REG_SYSCALL(id, syscall_enter);
#endif
   if (!in_system_mode)
   {
      SNP_REG_SYSCALL_RET(id, syscall_ret);
      g_clocks = new Clocks(MAX_VCPUS, clock_instructions, nullptr);
   }
   SNP_REG_VCPU_INIT(id, vcpu_init);
#if QEMU_PLUGIN_VERSION >= 7
   if (SNP_HAVE_CLAIM)
      qemu_plugin_register_vcpu_insn_claim_cb(id, insn_claim, nullptr);
#endif
   SNP_REG_VCPU_EXIT(id, vcpu_exit);
   SNP_REG_ATEXIT(id, at_exit);
   if (!in_system_mode)
      pthread_atfork(NULL, NULL, refuse_forked_child);
   return 0;
}
