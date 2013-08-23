#ifndef __GLOBALS_H
#define __GLOBALS_H

#define __STDC_FORMAT_MACROS
#include <inttypes.h>

#include "pin.H"
#ifdef PINPLAY_SUPPORTED
# include "pinplay.H"
#endif

#include <unordered_map>
#include <deque>

//#define DEBUG_OUTPUT 1
#define DEBUG_OUTPUT 0

#define LINE_SIZE_BYTES 64
#define MAX_NUM_SYSCALLS 4096
#define MAX_NUM_THREADS 128

extern KNOB<string> KnobOutputFile;
extern KNOB<UINT64> KnobBlocksize;
extern KNOB<UINT64> KnobUseROI;
extern KNOB<UINT64> KnobMPIImplicitROI;
extern KNOB<UINT64> KnobFastForwardTarget;
extern KNOB<UINT64> KnobDetailedTarget;
extern KNOB<UINT64> KnobUseResponseFiles;
extern KNOB<UINT64> KnobEmulateSyscalls;
extern KNOB<BOOL>   KnobSendPhysicalAddresses;
extern KNOB<UINT64> KnobFlowControl;
extern KNOB<INT64> KnobSiftAppId;
extern KNOB<BOOL> KnobRoutineTracing;
extern KNOB<BOOL> KnobRoutineTracingOutsideDetailed;
extern KNOB<BOOL> KnobDebug;
extern KNOB<BOOL> KnobVerbose;

# define KNOB_REPLAY_NAME "replay"
# define KNOB_FAMILY "pintool:sift-recorder"
extern KNOB_COMMENT pinplay_driver_knob_family;
extern KNOB<BOOL>KnobReplayer;
#ifdef PINPLAY_SUPPORTED
extern PINPLAY_ENGINE pinplay_engine;
#endif /* PINPLAY_SUPPORTED */

extern INT32 app_id;
extern INT32 num_threads;
extern UINT64 blocksize;
extern UINT64 fast_forward_target;
extern UINT64 detailed_target;
extern PIN_LOCK access_memory_lock;
extern PIN_LOCK new_threadid_lock;
extern std::deque<ADDRINT> tidptrs;
extern BOOL any_thread_in_detail;
extern const bool verbose;
extern std::unordered_map<ADDRINT, bool> routines;

#if defined(TARGET_IA32)
   typedef uint32_t syscall_args_t[6];
#elif defined(TARGET_INTEL64)
   typedef uint64_t syscall_args_t[6];
#endif

#endif // __GLOBALS_H
