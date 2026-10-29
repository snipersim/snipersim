#include "globals.h"

KNOB<std::string> KnobOutputFile(KNOB_MODE_WRITEONCE, "pintool", "sniper:o", "trace", "output");
KNOB<UINT64> KnobBlocksize(KNOB_MODE_WRITEONCE, "pintool", "sniper:b", "0", "blocksize");
KNOB<UINT64> KnobUseROI(KNOB_MODE_WRITEONCE, "pintool", "sniper:roi", "0", "use ROI markers");
KNOB<UINT64> KnobMPIImplicitROI(KNOB_MODE_WRITEONCE, "pintool", "sniper:roi-mpi", "0", "Implicit ROI between MPI_Init and MPI_Finalize");
KNOB<UINT64> KnobFastForwardTarget(KNOB_MODE_WRITEONCE, "pintool", "sniper:f", "0", "instructions to fast forward");
KNOB<UINT64> KnobDetailedTarget(KNOB_MODE_WRITEONCE, "pintool", "sniper:d", "0", "instructions to trace in detail (default = all)");
KNOB<UINT64> KnobUseResponseFiles(KNOB_MODE_WRITEONCE, "pintool", "sniper:r", "0", "use response files (required for multithreaded applications or when emulating syscalls, default = 0)");
KNOB<UINT64> KnobEmulateSyscalls(KNOB_MODE_WRITEONCE, "pintool", "sniper:e", "0", "emulate syscalls (required for multithreaded applications, default = 0)");
KNOB<BOOL>   KnobSendPhysicalAddresses(KNOB_MODE_WRITEONCE, "pintool", "sniper:pa", "0", "send logical to physical address mapping");
KNOB<BOOL>   KnobSmcSupport(KNOB_MODE_WRITEONCE, "pintool", "sniper:smc", "0", "strict self-modifying code support: compare every instruction's bytes with those last sent");
KNOB<BOOL>   KnobSmcTrack(KNOB_MODE_WRITEONCE, "pintool", "sniper:smc-track", "1", "track code rewritten in place, detected when Pin re-translates it, and send its pages again");
KNOB<UINT64> KnobFlowControl(KNOB_MODE_WRITEONCE, "pintool", "sniper:flow", "1000", "number of instructions to send before syncing up");
KNOB<UINT64> KnobFlowControlFF(KNOB_MODE_WRITEONCE, "pintool", "sniper:flowff", "100000", "number of instructions to batch up before sending instruction counts in fast-forward mode");
KNOB<INT64> KnobSiftAppId(KNOB_MODE_WRITEONCE, "pintool", "sniper:s", "0", "sift app id (default = 0)");
KNOB<BOOL> KnobRoutineTracing(KNOB_MODE_WRITEONCE, "pintool", "sniper:rtntrace", "0", "routine tracing");
KNOB<BOOL> KnobRoutineTracingOutsideDetailed(KNOB_MODE_WRITEONCE, "pintool", "sniper:rtntrace_outsidedetail", "0", "routine tracing");
KNOB<BOOL> KnobDebug(KNOB_MODE_WRITEONCE, "pintool", "sniper:debug", "0", "start debugger on internal exception");
KNOB<BOOL> KnobVerbose(KNOB_MODE_WRITEONCE, "pintool", "sniper:verbose", "0", "verbose output");
KNOB<INT64> KnobPacSimEnable(KNOB_MODE_WRITEONCE, "pintool", "sniper:pacsim", "0", "Pac-Sim online sampling: 0 off, 1 on");
KNOB<std::string> KnobPacSimDir(KNOB_MODE_WRITEONCE, "pintool", "sniper:pacsim-dir", ".", "directory for pacsim_regions.json");
KNOB<UINT64> KnobPacSimRegionSize(KNOB_MODE_WRITEONCE, "pintool", "sniper:pacsim-region-size", "50000", "sampled region size, in thousands of instructions summed over all threads (a region ends at the first loop back-edge after it)");
KNOB<UINT64> KnobPacSimMinRegionSize(KNOB_MODE_WRITEONCE, "pintool", "sniper:pacsim-min-region-size", "2000", "shorter regions join the next one (thousands of instructions)");
KNOB<double> KnobPacSimClusterThreshold(KNOB_MODE_WRITEONCE, "pintool", "sniper:pacsim-cluster-threshold", "0.2", "BBV distance below which two regions share a cluster");
KNOB<UINT64> KnobPacSimMinMeasurements(KNOB_MODE_WRITEONCE, "pintool", "sniper:pacsim-min-measurements", "2", "measure a cluster this many times before fast-forwarding its later instances (the first carries its warm-up)");
KNOB<BOOL> KnobPacSimWarmup(KNOB_MODE_WRITEONCE, "pintool", "sniper:pacsim-warmup", "1", "keep caches warm (cache-only mode) between measured regions instead of counting instructions only");
KNOB<BOOL> KnobTrackVMAs(KNOB_MODE_WRITEONCE, "pintool", "sniper:vma", "0", "track VMAs");

KNOB<UINT64> KnobStopAddress(KNOB_MODE_WRITEONCE, "pintool", "sniper:stop", "0", "stop address (0 = disabled)");
KNOB<UINT64> KnobMaxThreads(KNOB_MODE_WRITEONCE, "pintool", "sniper:maxthreads", "0", "maximum number of threads (0 = default)");

KNOB<UINT64> KnobTimeStart(KNOB_MODE_WRITEONCE, "pintool", "sniper:time_start", "1337000000", "the guest's time zero, seconds since the epoch: the simulator's osemu/time_start (record-trace --time-start)");
KNOB<BOOL> KnobPinballReplay(KNOB_MODE_WRITEONCE, "pintool", "sniper:pinball-replay", "0", "a pinball is being replayed (SDE's -replay; record-trace sets it): leave the guest's time to the pinball");

KNOB_COMMENT pinplay_driver_knob_family(KNOB_FAMILY, "PinPlay SIFT Recorder Knobs");
#if defined(SDE_INIT)
KNOB<BOOL>KnobReplayer(KNOB_MODE_WRITEONCE, KNOB_FAMILY,
					  "dummy", "0", "Replay a pinball");
#else
KNOB<BOOL>KnobReplayer(KNOB_MODE_WRITEONCE, KNOB_FAMILY,
                       KNOB_REPLAY_NAME, "0", "Replay a pinball");
#endif

KNOB<UINT64>KnobExtraePreLoaded(KNOB_MODE_WRITEONCE, "pintool", "sniper:extrae", "0", "Extrae preloaded");

#ifdef PINPLAY
PINPLAY_ENGINE *p_pinplay_engine;
#if !defined(SDE_INIT)
PINPLAY_ENGINE pp_pinplay_engine;
#endif /* !defined(SDE_INIT) */
#endif /* PINPLAY */

INT32 app_id;
INT32 num_threads = 0;
UINT32 max_num_threads = MAX_NUM_THREADS_DEFAULT;
UINT64 blocksize;
UINT64 fast_forward_target = 0;
UINT64 detailed_target = 0;
PIN_LOCK access_memory_lock;
PIN_LOCK new_threadid_lock;
std::deque<cloned_thread_t> tidptrs;
PIN_LOCK output_lock;
INT32 child_app_id = -1;
BOOL in_roi = false;
BOOL any_thread_in_detail = false;
Sift::Mode current_mode = Sift::ModeIcount;
std::unordered_map<ADDRINT, bool> routines;
std::ofstream *vma_output = NULL;

extrae_image_t extrae_image;
KNOB<std::string> KnobPacSimConverge(KNOB_MODE_WRITEONCE, "pintool", "sniper:pacsim-converge", "0.10", "fast-forward a cluster only once its last two measurements agree on the time per instruction to within this fraction (0: as soon as it has pacsim-min-measurements)");
KNOB<UINT64> KnobPacSimWarmupWindow(KNOB_MODE_WRITEONCE, "pintool", "sniper:pacsim-warmup-window", "0", "with warm-up between measured regions: warm only the last K thousand instructions of a fast-forwarded region, counting through the rest (0: warm the whole region)");
KNOB<UINT64> KnobPacSimLoopMin(KNOB_MODE_WRITEONCE, "pintool", "sniper:pacsim-loop-min", "1", "shortest loop iteration (instructions) whose back-edge may end a region: 1 = any back-edge (the paper's rule), 0 = 1% of the region size");
KNOB<std::string> KnobPacSimMarkers(KNOB_MODE_WRITEONCE, "pintool", "sniper:pacsim-markers", "cond", "backward branches that may end a region: cond (conditional only, the paper's rule) or all (unconditional jumps too)");
