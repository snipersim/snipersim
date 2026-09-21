#ifndef __SIM_API
#define __SIM_API

#define SIM_CMD_ROI_TOGGLE 0 // Deprecated, for compatibility with programs compiled long ago
#define SIM_CMD_ROI_START 1
#define SIM_CMD_ROI_END 2
#define SIM_CMD_MHZ_SET 3
#define SIM_CMD_MARKER 4
#define SIM_CMD_USER 5
#define SIM_CMD_INSTRUMENT_MODE 6
#define SIM_CMD_MHZ_GET 7
#define SIM_CMD_IN_SIMULATOR 8
#define SIM_CMD_PROC_ID 9
#define SIM_CMD_THREAD_ID 10
#define SIM_CMD_NUM_PROCS 11
#define SIM_CMD_NUM_THREADS 12
#define SIM_CMD_NAMED_MARKER 13
#define SIM_CMD_SET_THREAD_NAME 14
// Upstream extensions (from lchangxii PR)
#define SIM_CMD_GET_INS_NUM     15
#define SIM_CMD_GET_SIM_TIME    16
#define SIM_CMD_GET_BARRIER_REACHED 18
#define SIM_CMD_GET_L2_COUNT    1000
#define SIM_CMD_GET_CORE_TIME   1002   /* the calling core's elapsed time, fs */
#define SIM_CMD_GET_L2_MISS_COUNT 1001

// MimicOS userspace magic instructions
#define SIM_CMD_MALLOC          100
#define SIM_CMD_START_PROCESS   101
#define SIM_CMD_MIMICOS_RESULT  102
#define SIM_CMD_CONTEXT_SWITCH  103
#define SIM_CMD_RECEIVE_MESSAGE 104

/* Per-thread ROI barrier (Apr 15 2026).
   Coordinator calls SimRoiExpect(N) once to lock in how many threads must
   join before the perf model is enabled.  Each participating thread then
   calls SimThreadRoiStart() from its own context; perf model activates
   when the last one arrives.  Symmetric on teardown: perf model disables
   when the last thread has left. */
#define SIM_CMD_ROI_EXPECT 21
#define SIM_CMD_THREAD_ROI_START 22
#define SIM_CMD_THREAD_ROI_END 23
#define SIM_CMD_WAIT_FOR_ROI 24   // block until perf model is enabled
/* Stage 2B.2 (Apr 18 2026): schedule a user thread onto the calling core.
   Argument arg0 is an app_thread_id produced by AppReaderPool::encodeId(
   app_id, thread_num).  Handler installs the pool's Reader for arg0 as
   the caller core's app_reader + current_reader. */
#define SIM_CMD_CONTEXT_SWITCH_TO 25
/* Stage 3 (Apr 18 2026): return the app_id of the live app injected into
   the calling kernel pthread's TraceThread (or -1 if none).  MimicOS uses
   this right after spawn_live_application to learn the atid of the main
   thread and seed its per-core runqueue. */
#define SIM_CMD_GET_INJECTED_APP_ID 26
/* Phase 1 fast-fault replay (Apr 20 2026).
   MimicOS kernel ships a TRAINED fault profile to Sniper instead of
   executing the LinuxPhase helper chain locally.  arg0 = argc,
   arg1 = argv (uint64_t*).  Payload layout:
     argv[0] = SIM_CMD_FAST_FAULT (protocol marker, redundant with cmd)
     argv[1] = fp_id         (FastFaultProfile::fp_id)
     argv[2] = frame_pa      (physical address of the fault frame)
     argv[3] = mean_cycles   (uint64_t, already-rounded charge)
     argv[4] = trace_len     (entries in the memory_trace template)
     argv[5] = trace_ptr     (pointer to MemoryAccessTemplate[], guest VA)
     argv[6] = cursor        (monotonic counter for cold-line steering)
   Trace walking + cache-hierarchy drive lands in Phase 3 -- Phase 1
   only charges mean_cycles and returns. */
#define SIM_CMD_FAST_FAULT 27
/* Phase 5 (2026-04-22): set the fault-latency charge that the MMU
   should queue on the app thread's perf model when userspace MimicOS
   handles a fault.  arg0 = cycles (0 disables, falling back to the
   kernel-pthread-does-the-work path).  When set, the MMU advances
   barrier.global_time by this amount at fault time, freeing the
   kernel pthread to skip detailed LinuxPhase and fast-replay. */
#define SIM_CMD_SET_FAST_FAULT_CHARGE 28

#define SIM_OPT_INSTRUMENT_DETAILED 0
#define SIM_OPT_INSTRUMENT_WARMUP 1
#define SIM_OPT_INSTRUMENT_FASTFORWARD 2


#if defined(ARM_64)

#define SimMagic0(cmd) ({            \
   unsigned long _cmd = (cmd), _res; \
   asm volatile(                     \
       "mov x1, %[x]\n"              \
       "\tbfm x0, x0, 0, 0\n"        \
       : [ret] "=r"(_res)            \
       : [x] "r"(_cmd));             \
})

#define SimMagic1(cmd, arg0) ({                      \
   unsigned long _cmd = (cmd), _arg0 = (arg0), _res; \
   asm volatile(                                     \
       "mov x1, %[x]\n"                              \
       "\tmov x2, %[y]\n"                            \
       "\tbfm x0, x0, 0, 0\n"                        \
       : [ret] "=r"(_res)                            \
       : [x] "r"(_cmd),                              \
         [y] "r"(_arg0)                              \
       : "x2", "x1");                                \
})

#define SimMagic2(cmd, arg0, arg1) ({                                \
   unsigned long _cmd = (cmd), _arg0 = (arg0), _arg1 = (arg1), _res; \
   asm volatile(                                                     \
       "mov x1, %[x]\n"                                              \
       "\tmov x2, %[y]\n"                                            \
       "\tmov x3, %[z]\n"                                            \
       "\tbfm x0, x0, 0, 0\n"                                        \
       : [ret] "=r"(_res)                                            \
       : [x] "r"(_cmd),                                              \
         [y] "r"(_arg0),                                             \
         [z] "r"(_arg1)                                              \
       : "x1", "x2", "x3");                                          \
})

#else // end ARM_64

#if defined(__i386)
#define MAGIC_REG_A "eax"
#define MAGIC_REG_B "edx" // Required for -fPIC support
#define MAGIC_REG_C "ecx"
#else
#define MAGIC_REG_A "rax"
#define MAGIC_REG_B "rbx"
#define MAGIC_REG_C "rcx"
#endif

/* "=&a" rather than "=a" is required, not stylistic.  The asm writes
   MAGIC_REG_A from its first input before it has read the later ones, so
   without the earlyclobber the compiler may place an input in that same
   register and the first mov destroys it.  That is not theoretical: the
   userspace MimicOS kernel's fast-fault payload pointer was allocated to
   RAX, and the simulator received the command number where the pointer
   should have been -- argv read as seven zeroes from address 0x1b.
   Virtuoso sidesteps this by binding inputs to registers directly
   ("a", "b", "c") so no movs are needed at all. */
/* The "memory" clobber is load-bearing, not defensive.  Several commands pass
   a pointer and expect the simulator to read or write what it points at --
   SIM_CMD_FAST_FAULT reads a payload array, SIM_CMD_RECEIVE_MESSAGE writes
   one, SIM_CMD_START_PROCESS reads a path.  Without it the compiler is told
   only that some registers change, so stores into a local payload are dead
   (nothing visible reads them) and it is free to drop them.  At -O2 it does:
   the userspace MimicOS kernel's fast-fault smoke probe, which fills a
   seven-word payload and passes its address, arrived at the simulator as
   seven zeroes.  Virtuoso has the same omission. */
#define SimMagic0(cmd) ({            \
   unsigned long _cmd = (cmd), _res; \
   __asm__ __volatile__(             \
       "mov %1, %%" MAGIC_REG_A "\n" \
       "\txchg %%bx, %%bx\n"         \
       : "=&a"(_res) /* output    */  \
       : "g"(_cmd)  /* input     */  \
       : "memory"); /* clobbered */  \
   _res;                             \
})

#define SimMagic1(cmd, arg0) ({                      \
   unsigned long _cmd = (cmd), _arg0 = (arg0), _res; \
   __asm__ __volatile__(                             \
       "mov %1, %%" MAGIC_REG_A "\n"                 \
       "\tmov %2, %%" MAGIC_REG_B "\n"               \
       "\txchg %%bx, %%bx\n"                         \
       : "=&a"(_res) /* output    */                  \
       : "g"(_cmd),                                  \
         "g"(_arg0)        /* input     */           \
       : "%" MAGIC_REG_B, "memory"); /* clobbered */  \
   _res;                                             \
})

#define SimMagic2(cmd, arg0, arg1) ({                                \
   unsigned long _cmd = (cmd), _arg0 = (arg0), _arg1 = (arg1), _res; \
   __asm__ __volatile__(                                             \
       "mov %1, %%" MAGIC_REG_A "\n"                                 \
       "\tmov %2, %%" MAGIC_REG_B "\n"                               \
       "\tmov %3, %%" MAGIC_REG_C "\n"                               \
       "\txchg %%bx, %%bx\n"                                         \
       : "=&a"(_res) /* output    */                                  \
       : "g"(_cmd),                                                  \
         "g"(_arg0),                                                 \
         "g"(_arg1)                         /* input     */          \
       : "%" MAGIC_REG_B, "%" MAGIC_REG_C, "memory"); /* clobbered */ \
   _res;                                                             \
})

#endif

#define SimRoiStart() SimMagic0(SIM_CMD_ROI_START)
#define SimRoiEnd() SimMagic0(SIM_CMD_ROI_END)
#define SimGetProcId() SimMagic0(SIM_CMD_PROC_ID)
#define SimGetThreadId() SimMagic0(SIM_CMD_THREAD_ID)
#define SimSetThreadName(name) SimMagic1(SIM_CMD_SET_THREAD_NAME, (unsigned long)(name))
#define SimMalloc(size) SimMagic1(SIM_CMD_MALLOC, (unsigned long)(size))
#define SimGetNumProcs() SimMagic0(SIM_CMD_NUM_PROCS)
#define SimGetNumThreads() SimMagic0(SIM_CMD_NUM_THREADS)
#define SimSetFreqMHz(proc, mhz) SimMagic2(SIM_CMD_MHZ_SET, proc, mhz)
#define SimSetOwnFreqMHz(mhz) SimSetFreqMHz(SimGetProcId(), mhz)
#define SimGetFreqMHz(proc) SimMagic1(SIM_CMD_MHZ_GET, proc)
#define SimGetOwnFreqMHz() SimGetFreqMHz(SimGetProcId())
#define SimMarker(arg0, arg1) SimMagic2(SIM_CMD_MARKER, arg0, arg1)
#define SimNamedMarker(arg0, str) SimMagic2(SIM_CMD_NAMED_MARKER, arg0, (unsigned long)(str))
#define SimUser(cmd, arg) SimMagic2(SIM_CMD_USER, cmd, arg)
#define SimSetInstrumentMode(opt) SimMagic1(SIM_CMD_INSTRUMENT_MODE, opt)
#define SimInSimulator() (SimMagic0(SIM_CMD_IN_SIMULATOR) != SIM_CMD_IN_SIMULATOR)
#define SimStartProcess(message) SimMagic1(SIM_CMD_START_PROCESS, message)
#define SimMimicosResult(argc,argv) SimMagic2(SIM_CMD_MIMICOS_RESULT, (unsigned long)(argc), (unsigned long)(argv))
#define SimContextSwitch() SimMagic0(SIM_CMD_CONTEXT_SWITCH)
#define SimReceiveMessage(argc, argv) SimMagic2(SIM_CMD_RECEIVE_MESSAGE, (unsigned long)(argc), (unsigned long)(argv))
/* Per-thread ROI barrier: coordinator pre-declares thread count, then
   each participating thread joins/leaves from its own context.
   `id` is a caller-chosen opaque handle; pick any unique value per
   participant and reuse it for both start and end.  (PID would drift
   under syscall emulation, so the handle is explicit.) */
#define SimRoiExpect(n) SimMagic1(SIM_CMD_ROI_EXPECT, (unsigned long)(n))
#define SimThreadRoiStart(id) SimMagic1(SIM_CMD_THREAD_ROI_START, (unsigned long)(id))
#define SimThreadRoiEnd(id) SimMagic1(SIM_CMD_THREAD_ROI_END, (unsigned long)(id))
#define SimWaitForRoi() SimMagic0(SIM_CMD_WAIT_FOR_ROI)
/* Stage 2B.2 (Apr 18 2026): schedule user thread `app_thread_id` onto the
   calling core.  Handler looks up the Reader in AppReaderPool and installs
   it as the current core's app_reader + current_reader.  Returns 42. */
#define SimContextSwitchTo(app_thread_id) \
   SimMagic1(SIM_CMD_CONTEXT_SWITCH_TO, (unsigned long)(app_thread_id))
/* Stage 3 (Apr 18 2026): returns the app_id of the live app whose reader
   is currently injected into the calling kernel pthread's TraceThread.
   (UInt64)-1 if no live app is injected. */
#define SimGetInjectedAppId() SimMagic0(SIM_CMD_GET_INJECTED_APP_ID)
/* Phase 1 fast-fault replay (Apr 20 2026) -- see SIM_CMD_FAST_FAULT above. */
#define SimFastFault(argc, argv) SimMagic2(SIM_CMD_FAST_FAULT, (unsigned long)(argc), (unsigned long)(argv))
#define SimSetFastFaultCharge(cycles) SimMagic1(SIM_CMD_SET_FAST_FAULT_CHARGE, (unsigned long)(cycles))

#endif /* __SIM_API */
