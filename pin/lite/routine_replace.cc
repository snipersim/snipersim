#include "lite/routine_replace.h"
#include "instruction_modeling.h"
#include "pthread_emu.h"
#include "simulator.h"
#include "sync_api.h"
#include "performance_model.h"
#include "thread_manager.h"
#include "core_manager.h"
#include "core.h"
#include "thread.h"
#include "log.h"
#include "network.h"
#include "packet_type.h"
#include "magic_client.h"
#include "local_storage.h"
#include "trace_rtn.h"
#include "memory_tracker.h"

#include <map>
#include <cerrno>

// The Pintool can easily read from application memory, so
// we dont need to explicitly initialize stuff and do a special ret

namespace lite
{

std::unordered_map<core_id_t, SubsecondTime> pthread_t_start;
AFUNPTR ptr_exit = NULL;

struct pthread_functions_t {
   String name;
   PthreadEmu::pthread_enum_t function;
   PthreadEmu::state_t state_after;
} pthread_functions[] = {
   { "pthread_mutex_lock",      PthreadEmu::PTHREAD_MUTEX_LOCK,     PthreadEmu::STATE_INREGION },
   { "pthread_mutex_trylock",   PthreadEmu::PTHREAD_MUTEX_TRYLOCK,  PthreadEmu::STATE_BY_RETURN },
   { "pthread_mutex_unlock",    PthreadEmu::PTHREAD_MUTEX_UNLOCK,   PthreadEmu::STATE_RUNNING },
   { "pthread_cond_wait",       PthreadEmu::PTHREAD_COND_WAIT,      PthreadEmu::STATE_RUNNING },
   { "pthread_cond_signal",     PthreadEmu::PTHREAD_COND_SIGNAL,    PthreadEmu::STATE_RUNNING },
   { "pthread_cond_broadcast",  PthreadEmu::PTHREAD_COND_BROADCAST, PthreadEmu::STATE_RUNNING },
   { "pthread_barrier_wait",    PthreadEmu::PTHREAD_BARRIER_WAIT,   PthreadEmu::STATE_RUNNING },
};

void printStackTrace(THREADID threadid, char * function, BOOL enter)
{
   printf("[%u] %s %s\n", threadid, function, enter ? "Enter" : "Exit");
}

void routineStartCallback(RTN rtn, INS ins)
{
   String rtn_name = RTN_Name(rtn).c_str();

   // Routine instrumentation functions which can cause a rescheduling, or a jump in application code,
   // need to be called *before* the handleBasicBlock for any code in the routine,
   // else, we would first issue the basic block to one core and later the send the
   // dynamic information to another core, or send dynamic instructions for the wrong basic block.

   // icc/openmp compatibility
   if (rtn_name == "__kmp_reap_monitor" || rtn_name == "__kmp_internal_end_atexit")
   {
      INS_InsertCall(ins, IPOINT_BEFORE, AFUNPTR(emuKmpReapMonitor), IARG_THREAD_ID, IARG_CONTEXT, IARG_END);
   }
}

void routineCallback(RTN rtn, void* v)
{
   String rtn_name = RTN_Name(rtn).c_str();

   addRtnTracer(rtn);

   if (0)
   {
      RTN_Open (rtn);
      const char * name = (new String(rtn_name))->c_str();
      RTN_InsertCall (rtn, IPOINT_BEFORE, AFUNPTR (printStackTrace), IARG_THREAD_ID, IARG_ADDRINT, name, IARG_BOOL, true, IARG_END);
      RTN_InsertCall (rtn, IPOINT_AFTER,  AFUNPTR (printStackTrace), IARG_THREAD_ID, IARG_ADDRINT, name, IARG_BOOL, false, IARG_END);
      RTN_Close (rtn);
   }

   if (Sim()->getMemoryTracker())
   {
      if (rtn_name == "malloc" || rtn_name == "_int_malloc")
      {
         int size_pos = 0;
         if (rtn_name == "_int_malloc") size_pos = 1;

         RTN_Open(rtn);
         RTN_InsertCall(rtn, IPOINT_BEFORE, AFUNPTR(mallocBefore), IARG_THREAD_ID, IARG_RETURN_IP, IARG_FUNCARG_ENTRYPOINT_VALUE, size_pos, IARG_END);
         RTN_InsertCall(rtn, IPOINT_AFTER,  AFUNPTR(mallocAfter), IARG_THREAD_ID, IARG_FUNCRET_EXITPOINT_VALUE, IARG_END);
         RTN_Close(rtn);
      }
      if (rtn_name == "free")
      {
         RTN_Open(rtn);
         RTN_InsertCall(rtn, IPOINT_BEFORE, AFUNPTR(freeBefore), IARG_THREAD_ID, IARG_RETURN_IP, IARG_FUNCARG_ENTRYPOINT_VALUE, 0, IARG_END);
         RTN_Close(rtn);
      }
   }

   // save pointers to some functions we'll want to call through PIN_CallApplicationFunction
   if (rtn_name == "exit")                   ptr_exit = RTN_Funptr(rtn);

   ////////////////////////////////////////
   // Above this point: monitor-only instrumentation that is safe to use with PinPlay
   ////////////////////////////////////////

   if (!Sim()->getConfig()->getEnableSyscallEmulation())
      return;

   ////////////////////////////////////////
   // Below this point: emulation code that is NOT safe with PinPlay
   ////////////////////////////////////////

   // CarbonStartSim() and CarbonStopSim()
   if (rtn_name == "CarbonStartSim")
   {
      PROTO proto = PROTO_Allocate(PIN_PARG(SInt32),
            CALLINGSTD_DEFAULT,
            "CarbonStartSim",
            PIN_PARG(int),
            PIN_PARG(char**),
            PIN_PARG_END());

      RTN_ReplaceSignature(rtn,
            AFUNPTR(lite::nullFunction),
            IARG_PROTOTYPE, proto,
            IARG_END);
   }
   else if (rtn_name == "CarbonStopSim")
   {
      PROTO proto = PROTO_Allocate(PIN_PARG(void),
            CALLINGSTD_DEFAULT,
            "CarbonStopSim",
            PIN_PARG_END());

      RTN_ReplaceSignature(rtn,
            AFUNPTR(lite::nullFunction),
            IARG_PROTOTYPE, proto,
            IARG_END);
   }

   // Synchronization
   else if (rtn_name == "CarbonMutexInit")
   {
      PROTO proto = PROTO_Allocate(PIN_PARG(void),
            CALLINGSTD_DEFAULT,
            "CarbonMutexInit",
            PIN_PARG(carbon_mutex_t*),
            PIN_PARG_END());

      RTN_ReplaceSignature(rtn,
            AFUNPTR(CarbonMutexInit),
            IARG_PROTOTYPE, proto,
            IARG_FUNCARG_ENTRYPOINT_VALUE, 0,
            IARG_END);
   }
   else if (rtn_name == "CarbonMutexLock")
   {
      PROTO proto = PROTO_Allocate(PIN_PARG(void),
            CALLINGSTD_DEFAULT,
            "CarbonMutexLock",
            PIN_PARG(carbon_mutex_t*),
            PIN_PARG_END());

      RTN_ReplaceSignature(rtn,
            AFUNPTR(CarbonMutexLock),
            IARG_PROTOTYPE, proto,
            IARG_FUNCARG_ENTRYPOINT_VALUE, 0,
            IARG_END);
   }
   else if (rtn_name == "CarbonMutexUnlock")
   {
      PROTO proto = PROTO_Allocate(PIN_PARG(void),
            CALLINGSTD_DEFAULT,
            "CarbonMutexUnlock",
            PIN_PARG(carbon_mutex_t*),
            PIN_PARG_END());

      RTN_ReplaceSignature(rtn,
            AFUNPTR(CarbonMutexUnlock),
            IARG_PROTOTYPE, proto,
            IARG_FUNCARG_ENTRYPOINT_VALUE, 0,
            IARG_END);
   }
   else if (rtn_name == "CarbonCondInit")
   {
      PROTO proto = PROTO_Allocate(PIN_PARG(void),
            CALLINGSTD_DEFAULT,
            "CarbonCondInit",
            PIN_PARG(carbon_cond_t*),
            PIN_PARG_END());

      RTN_ReplaceSignature(rtn,
            AFUNPTR(CarbonCondInit),
            IARG_PROTOTYPE, proto,
            IARG_FUNCARG_ENTRYPOINT_VALUE, 0,
            IARG_END);
   }
   else if (rtn_name == "CarbonCondWait")
   {
      PROTO proto = PROTO_Allocate(PIN_PARG(void),
            CALLINGSTD_DEFAULT,
            "CarbonCondWait",
            PIN_PARG(carbon_cond_t*),
            PIN_PARG(carbon_mutex_t*),
            PIN_PARG_END());

      RTN_ReplaceSignature(rtn,
            AFUNPTR(CarbonCondWait),
            IARG_PROTOTYPE, proto,
            IARG_FUNCARG_ENTRYPOINT_VALUE, 0,
            IARG_FUNCARG_ENTRYPOINT_VALUE, 1,
            IARG_END);
   }
   else if (rtn_name == "CarbonCondSignal")
   {
      PROTO proto = PROTO_Allocate(PIN_PARG(void),
            CALLINGSTD_DEFAULT,
            "CarbonCondSignal",
            PIN_PARG(carbon_cond_t*),
            PIN_PARG_END());

      RTN_ReplaceSignature(rtn,
            AFUNPTR(CarbonCondSignal),
            IARG_PROTOTYPE, proto,
            IARG_FUNCARG_ENTRYPOINT_VALUE, 0,
            IARG_END);
   }
   else if (rtn_name == "CarbonCondBroadcast")
   {
      PROTO proto = PROTO_Allocate(PIN_PARG(void),
            CALLINGSTD_DEFAULT,
            "CarbonCondBroadcast",
            PIN_PARG(carbon_cond_t*),
            PIN_PARG_END());

      RTN_ReplaceSignature(rtn,
            AFUNPTR(CarbonCondBroadcast),
            IARG_PROTOTYPE, proto,
            IARG_FUNCARG_ENTRYPOINT_VALUE, 0,
            IARG_END);
   }
   else if (rtn_name == "CarbonBarrierInit")
   {
      PROTO proto = PROTO_Allocate(PIN_PARG(void),
            CALLINGSTD_DEFAULT,
            "CarbonBarrierInit",
            PIN_PARG(carbon_barrier_t*),
            PIN_PARG(unsigned int),
            PIN_PARG_END());

      RTN_ReplaceSignature(rtn,
            AFUNPTR(CarbonBarrierInit),
            IARG_PROTOTYPE, proto,
            IARG_FUNCARG_ENTRYPOINT_VALUE, 0,
            IARG_FUNCARG_ENTRYPOINT_VALUE, 1,
            IARG_END);
   }
   else if (rtn_name == "CarbonBarrierWait")
   {
      PROTO proto = PROTO_Allocate(PIN_PARG(void),
            CALLINGSTD_DEFAULT,
            "CarbonBarrierWait",
            PIN_PARG(carbon_barrier_t*),
            PIN_PARG_END());

      RTN_ReplaceSignature(rtn,
            AFUNPTR(CarbonBarrierWait),
            IARG_PROTOTYPE, proto,
            IARG_FUNCARG_ENTRYPOINT_VALUE, 0,
            IARG_END);
   }

   // os emulation
   else if (rtn_name == "sched_getcpu")      RTN_ReplaceSignature(rtn, AFUNPTR(emuGetCPU), IARG_THREAD_ID, IARG_END);
   else if (rtn_name == "get_nprocs"      || rtn_name == "__get_nprocs")
      RTN_Replace(rtn, AFUNPTR(emuGetNprocs));
   else if (rtn_name == "get_nprocs_conf" || rtn_name == "__get_nprocs_conf")
      RTN_Replace(rtn, AFUNPTR(emuGetNprocs));
   if (Sim()->getConfig()->getOSEmuClockReplace())
   {
      if (rtn_name == "clock_gettime")
         RTN_ReplaceSignature(rtn, AFUNPTR(emuClockGettime), IARG_THREAD_ID,
                              IARG_FUNCARG_ENTRYPOINT_VALUE, 0, IARG_FUNCARG_ENTRYPOINT_VALUE, 1, IARG_END);
      if (rtn_name.find("gettimeofday") != String::npos)
         RTN_ReplaceSignature(rtn, AFUNPTR(emuGettimeofday), IARG_THREAD_ID,
                              IARG_FUNCARG_ENTRYPOINT_VALUE, 0, IARG_FUNCARG_ENTRYPOINT_VALUE, 1, IARG_END);
   }

   if (Sim()->getConfig()->getOSEmuPthreadReplace()) {
      if (rtn_name.find("pthread_mutex_init") != String::npos)      RTN_Replace(rtn, AFUNPTR(PthreadEmu::MutexInit));
      else if (rtn_name.find("pthread_mutex_lock") != String::npos)      RTN_Replace(rtn, AFUNPTR(PthreadEmu::MutexLock));
      else if (rtn_name.find("pthread_mutex_trylock") != String::npos)   RTN_Replace(rtn, AFUNPTR(PthreadEmu::MutexTrylock));
      else if (rtn_name.find("pthread_mutex_unlock") != String::npos)    RTN_Replace(rtn, AFUNPTR(PthreadEmu::MutexUnlock));
      else if (rtn_name.find("pthread_mutex_destroy") != String::npos)   RTN_Replace(rtn, AFUNPTR(nullFunction));
      else if (rtn_name.find("pthread_cond_init") != String::npos)       RTN_Replace(rtn, AFUNPTR(PthreadEmu::CondInit));
      else if (rtn_name.find("pthread_cond_wait") != String::npos)       RTN_Replace(rtn, AFUNPTR(PthreadEmu::CondWait));
      else if (rtn_name.find("pthread_cond_signal") != String::npos)     RTN_Replace(rtn, AFUNPTR(PthreadEmu::CondSignal));
      else if (rtn_name.find("pthread_cond_broadcast") != String::npos)  RTN_Replace(rtn, AFUNPTR(PthreadEmu::CondBroadcast));
      else if (rtn_name.find("pthread_cond_destroy") != String::npos)    RTN_Replace(rtn, AFUNPTR(nullFunction));
      else if (rtn_name.find("pthread_barrier_init") != String::npos)    RTN_Replace(rtn, AFUNPTR(PthreadEmu::BarrierInit));
      else if (rtn_name.find("pthread_barrier_wait") != String::npos)    RTN_Replace(rtn, AFUNPTR(PthreadEmu::BarrierWait));
      else if (rtn_name.find("pthread_barrier_destroy") != String::npos) RTN_Replace(rtn, AFUNPTR(nullFunction));
   } else {
      // pthread wrappers
      for(unsigned int i = 0; i < sizeof(pthread_functions) / sizeof(pthread_functions_t); ++i) {
         if (rtn_name.find(pthread_functions[i].name) != String::npos) {
            RTN_Open(rtn);
            RTN_InsertCall(rtn, IPOINT_BEFORE, AFUNPTR(pthreadBefore), IARG_THREAD_ID, IARG_END);
            RTN_InsertCall(rtn, IPOINT_AFTER,  AFUNPTR(pthreadAfter), IARG_THREAD_ID, IARG_ADDRINT, i, IARG_FUNCRET_EXITPOINT_VALUE, IARG_END);
            RTN_Close(rtn);
         }
      }
   }
}

AFUNPTR getFunptr(CONTEXT* context, string func_name)
{
   IntPtr reg_inst_ptr = PIN_GetContextReg(context, REG_INST_PTR);

   PIN_LockClient();
   IMG img = IMG_FindByAddress(reg_inst_ptr);
   RTN rtn = RTN_FindByName(img, func_name.c_str());
   PIN_UnlockClient();

   return RTN_Funptr(rtn);
}

IntPtr nullFunction()
{
   LOG_PRINT("In nullFunction()");
   return IntPtr(0);
}

void pthreadBefore(THREADID thread_id)
{
   Core *core = localStore[thread_id].thread->getCore();
   assert(core);
   pthread_t_start[thread_id] = core->getPerformanceModel()->getElapsedTime();
   updateState(core, PthreadEmu::STATE_WAITING);
}

void pthreadAfter(THREADID thread_id, ADDRINT type_id, ADDRINT retval)
{
   Core *core = localStore[thread_id].thread->getCore();
   assert(core);
   PthreadEmu::state_t new_state;
   if (pthread_functions[type_id].state_after == PthreadEmu::STATE_BY_RETURN)
      new_state = retval == EBUSY ? PthreadEmu::STATE_RUNNING : PthreadEmu::STATE_INREGION;
   else
      new_state = pthread_functions[type_id].state_after;
   updateState(core, new_state);
   pthreadCount(pthread_functions[type_id].function, core, core->getPerformanceModel()->getElapsedTime() - pthread_t_start[thread_id], SubsecondTime::Zero());
}

void mallocBefore(THREADID thread_id, ADDRINT eip, ADDRINT size)
{
   localStore[thread_id].malloc.eip = localStore[thread_id].lastCallSite;
   localStore[thread_id].malloc.size = size;
}

void mallocAfter(THREADID thread_id, ADDRINT address)
{
   if (localStore[thread_id].malloc.size)
   {
      Sim()->getMemoryTracker()->logMalloc(localStore[thread_id].thread->getId(), localStore[thread_id].malloc.eip, address, localStore[thread_id].malloc.size);
      localStore[thread_id].malloc.size = 0;
   }
}

void freeBefore(THREADID thread_id, ADDRINT eip, ADDRINT address)
{
   Sim()->getMemoryTracker()->logFree(localStore[thread_id].thread->getId(), eip, address);
}

IntPtr emuGetNprocs()
{
   return Sim()->getConfig()->getOSEmuNprocs()
   ? Sim()->getConfig()->getOSEmuNprocs()
   : Sim()->getConfig()->getApplicationCores();
}

IntPtr emuGetCPU(THREADID thread_id)
{
   Core *core = localStore[thread_id].thread->getCore();
   assert(core);
   return core->getId();
}

IntPtr emuClockGettime(THREADID thread_id, clockid_t clk_id, struct timespec *tp)
{
   switch(clk_id)
   {
      case CLOCK_REALTIME:
      case CLOCK_MONOTONIC:
         // Return simulated time
         if (tp)
         {
            Core *core = localStore[thread_id].thread->getCore();
            assert(core);
            UInt64 time = Sim()->getConfig()->getOSEmuTimeStart() * 1000000000
                        + core->getPerformanceModel()->getElapsedTime().getNS();

            tp->tv_sec = time / 1000000000;
            tp->tv_nsec = time % 1000000000;
         }
         return 0;
      default:
         // Unknown/non-emulated clock types (such as CLOCK_PROCESS_CPUTIME_ID/CLOCK_THREAD_CPUTIME_ID)
         return clock_gettime(clk_id, tp);
   }
}

IntPtr emuGettimeofday(THREADID thread_id, struct timeval *tv, struct timezone *tz)
{
   LOG_ASSERT_WARNING_ONCE(tz == NULL, "gettimeofday() with non-NULL timezone not supported");
   LOG_ASSERT_ERROR(tv != NULL, "gettimeofday() called with NULL timeval not supported");

   Core *core = localStore[thread_id].thread->getCore();
   assert(core);
   UInt64 time = Sim()->getConfig()->getOSEmuTimeStart() * 1000000000
               + core->getPerformanceModel()->getElapsedTime().getNS();

   tv->tv_sec = time / 1000000000;
   tv->tv_usec = (time / 1000) % 1000000;

   return 0;
}

void emuKmpReapMonitor(THREADID threadIndex, CONTEXT *ctxt)
{
   // Hack to make ICC's OpenMP runtime library work.
   // This runtime creates a monitor thread which blocks in a condition variable with a timeout.
   // On exit, thread 0 executes __kmp_reap_monitor() which join()s on this monitor thread.
   // In due time, the timeout occurs and the monitor thread returns
   // from pthread_cond_timedwait(), sees that it should be exiting, and returns.
   // However, in simulation all of this happens post-ROI, where time is not advancing so the timeout never occurs.
   // Having time advance using a one-IPC model during pre- and post-ROI would be nice, but for now,
   // just forcefully terminate the application when the master thread reaches __kmp_reap_monitor().
   void *res;
#if PIN_REV >= 71313
   PIN_CallApplicationFunction(ctxt, threadIndex, CALLINGSTD_DEFAULT, ptr_exit, NULL, PIN_PARG(void*), &res, PIN_PARG(int), 0, PIN_PARG_END());
#else
   PIN_CallApplicationFunction(ctxt, threadIndex, CALLINGSTD_DEFAULT, ptr_exit, PIN_PARG(void*), &res, PIN_PARG(int), 0, PIN_PARG_END());
#endif
}

}
