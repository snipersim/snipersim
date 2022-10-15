#ifndef __GLOBALS_H
#define __GLOBALS_H

#include "sift_format.h"

#include "pin.H"
#if defined(SDE_INIT)
#include "sde-pinplay-supp.H"
#endif
#ifdef PINPLAY
# include "pinplay.H"
#endif
#include "control_manager.H"
#include <unordered_map>
#include <deque>

//#define DEBUG_OUTPUT 1
#define DEBUG_OUTPUT 0

#define LINE_SIZE_BYTES 64
#define MAX_NUM_SYSCALLS 4096
#define MAX_NUM_THREADS_DEFAULT 128

extern KNOB<std::string> KnobOutputFile;
extern KNOB<UINT64> KnobBlocksize;
extern KNOB<UINT64> KnobUseROI;
extern KNOB<UINT64> KnobMPIImplicitROI;
extern KNOB<UINT64> KnobFastForwardTarget;
extern KNOB<UINT64> KnobDetailedTarget;
extern KNOB<UINT64> KnobUseResponseFiles;
extern KNOB<UINT64> KnobEmulateSyscalls;
extern KNOB<BOOL>   KnobSendPhysicalAddresses;
extern KNOB<UINT64> KnobFlowControl;
extern KNOB<UINT64> KnobFlowControlFF;
extern KNOB<INT64> KnobSiftAppId;
extern KNOB<BOOL> KnobRoutineTracing;
extern KNOB<BOOL> KnobRoutineTracingOutsideDetailed;
extern KNOB<BOOL> KnobDebug;
extern KNOB<BOOL> KnobVerbose;
extern KNOB<UINT64> KnobStopAddress;
extern KNOB<UINT64> KnobMaxThreads;
extern KNOB<UINT64> KnobExtraePreLoaded;

# define KNOB_REPLAY_NAME "replay"
# define KNOB_FAMILY "pintool:sift-recorder"
extern KNOB_COMMENT pinplay_driver_knob_family;
extern KNOB<BOOL>KnobReplayer;
#ifdef PINPLAY
extern PINPLAY_ENGINE *p_pinplay_engine;
extern PINPLAY_ENGINE pp_pinplay_engine;
#endif /* PINPLAY */
extern INT32 app_id;
extern INT32 num_threads;
extern UINT32 max_num_threads;
extern UINT64 blocksize;
extern UINT64 fast_forward_target;
extern UINT64 detailed_target;
extern PIN_LOCK access_memory_lock;
extern PIN_LOCK new_threadid_lock;
extern PIN_LOCK output_lock;
extern std::deque<ADDRINT> tidptrs;
extern INT32 child_app_id;
extern BOOL in_roi;
extern BOOL any_thread_in_detail;
extern Sift::Mode current_mode;
extern const bool verbose;
extern std::unordered_map<ADDRINT, bool> routines;

struct extrae_image_t {
  ADDRINT top_addr;
  ADDRINT bottom_addr;
  BOOL linked;
  BOOL got_init;
  BOOL got_fini;
};

extern extrae_image_t extrae_image;

typedef uint64_t syscall_args_t[6];
/*
#if defined(TARGET_IA32)
   typedef uint32_t syscall_args_t[6];
#elif defined(TARGET_INTEL64)
   typedef uint64_t syscall_args_t[6];
#endif
*/

#endif // __GLOBALS_H
