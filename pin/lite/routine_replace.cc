#include "lite/routine_replace.h"
#include "instruction_modeling.h"
#include "pthread_emu.h"
#include "simulator.h"
#include "sync_api.h"
#include "performance_model.h"
#include "thread_manager.h"
#include "core_manager.h"
#include "core.h"
#include "log.h"
#include "network.h"
#include "packet_type.h"
#include "magic_client.h"
#include "local_storage.h"

#include <map>
#include <cerrno>

// The Pintool can easily read from application memory, so
// we dont need to explicitly initialize stuff and do a special ret

namespace lite
{

multimap<core_id_t, pthread_t> tid_to_thread_ptr_map;
std::unordered_map<core_id_t, SubsecondTime> pthread_t_start;
AFUNPTR pthread_create_func = NULL, pthread_join_func = NULL;

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

void routineCallback(RTN rtn, void* v)
{
   String rtn_name = RTN_Name(rtn).c_str();

   if (0) {
      RTN_Open (rtn);
      const char * name = (new String(rtn_name))->c_str();
      RTN_InsertCall (rtn, IPOINT_BEFORE, AFUNPTR (printStackTrace), IARG_THREAD_ID, IARG_ADDRINT, name, IARG_BOOL, true, IARG_END);
      RTN_InsertCall (rtn, IPOINT_AFTER,  AFUNPTR (printStackTrace), IARG_THREAD_ID, IARG_ADDRINT, name, IARG_BOOL, false, IARG_END);
      RTN_Close (rtn);
   }


   // If we find pthread_create/pthread_join somewhere, save their pointer
   if (rtn_name.find("pthread_create") != String::npos)
      pthread_create_func = RTN_Funptr(rtn);
   else if (rtn_name.find("pthread_join") != String::npos)
      pthread_join_func = RTN_Funptr(rtn);


   // main
   if (rtn_name == "main")
   {
      if (! Sim()->getConfig()->useMagic()) {
         RTN_Open(rtn);

         RTN_InsertCall(rtn, IPOINT_BEFORE,
               AFUNPTR(enablePerformanceGlobal),
               IARG_END);

         RTN_InsertCall(rtn, IPOINT_AFTER,
               AFUNPTR(disablePerformanceGlobal),
               IARG_END);

         RTN_Close(rtn);
      }
   }
   // or, when the application explicitly calls exit(), do it there
   if (rtn_name == "exit")
   {
      if (! Sim()->getConfig()->useMagic()) {
         RTN_Open (rtn);
         RTN_InsertCall (rtn, IPOINT_BEFORE,
               AFUNPTR(disablePerformanceGlobal),
               IARG_END);
         RTN_Close (rtn);
      }
   }

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

   // Thread Creation
   else if (rtn_name == "CarbonSpawnThread")
   {
      PROTO proto = PROTO_Allocate(PIN_PARG(carbon_thread_t),
            CALLINGSTD_DEFAULT,
            "CarbonSpawnThread",
            PIN_PARG(thread_func_t),
            PIN_PARG(void*),
            PIN_PARG_END());

      RTN_ReplaceSignature(rtn,
            AFUNPTR(lite::emuCarbonSpawnThread),
            IARG_PROTOTYPE, proto,
            IARG_CONTEXT,
            IARG_FUNCARG_ENTRYPOINT_VALUE, 0,
            IARG_FUNCARG_ENTRYPOINT_VALUE, 1,
            IARG_END);
   }
   else if (rtn_name.find("pthread_create") != string::npos)
   {
      RTN_Open(rtn);
      RTN_InsertCall(rtn, IPOINT_BEFORE, AFUNPTR(emuPthreadCreateBefore), IARG_THREAD_ID,
         IARG_FUNCARG_ENTRYPOINT_VALUE, 0, IARG_FUNCARG_ENTRYPOINT_VALUE, 2, IARG_FUNCARG_ENTRYPOINT_VALUE, 3, IARG_END);
      RTN_InsertCall(rtn, IPOINT_AFTER, AFUNPTR(emuPthreadCreateAfter), IARG_THREAD_ID, IARG_END);
      RTN_Close(rtn);
   }
   // Thread Joining
   else if (rtn_name == "CarbonJoinThread")
   {
      PROTO proto = PROTO_Allocate(PIN_PARG(void),
            CALLINGSTD_DEFAULT,
            "CarbonJoinThread",
            PIN_PARG(carbon_thread_t),
            PIN_PARG_END());

      RTN_ReplaceSignature(rtn,
            AFUNPTR(lite::emuCarbonJoinThread),
            IARG_PROTOTYPE, proto,
            IARG_CONTEXT,
            IARG_FUNCARG_ENTRYPOINT_VALUE, 0,
            IARG_END);
   }
   else if (rtn_name.find("pthread_join") != string::npos)
   {
      RTN_Open(rtn);
      RTN_InsertCall(rtn, IPOINT_BEFORE, AFUNPTR(emuPthreadJoinBefore), IARG_FUNCARG_ENTRYPOINT_VALUE, 0, IARG_END);
      RTN_Close(rtn);
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

   // CAPI Functions
   else if (rtn_name == "CAPI_Initialize")
   {
      PROTO proto = PROTO_Allocate(PIN_PARG(CAPI_return_t),
            CALLINGSTD_DEFAULT,
            "CAPI_Initialize",
            PIN_PARG(int),
            PIN_PARG_END());

      RTN_ReplaceSignature(rtn,
            AFUNPTR(CAPI_Initialize),
            IARG_PROTOTYPE, proto,
            IARG_FUNCARG_ENTRYPOINT_VALUE, 0,
            IARG_END);
   }
   else if (rtn_name == "CAPI_rank")
   {
      PROTO proto = PROTO_Allocate(PIN_PARG(CAPI_return_t),
            CALLINGSTD_DEFAULT,
            "CAPI_rank",
            PIN_PARG(int*),
            PIN_PARG_END());

      RTN_ReplaceSignature(rtn,
            AFUNPTR(CAPI_rank),
            IARG_PROTOTYPE, proto,
            IARG_FUNCARG_ENTRYPOINT_VALUE, 0,
            IARG_END);
   }
   else if (rtn_name == "CAPI_message_send_w")
   {
      PROTO proto = PROTO_Allocate(PIN_PARG(CAPI_return_t),
            CALLINGSTD_DEFAULT,
            "CAPI_message_send_w",
            PIN_PARG(CAPI_endpoint_t),
            PIN_PARG(CAPI_endpoint_t),
            PIN_PARG(char*),
            PIN_PARG(int),
            PIN_PARG_END());

      RTN_ReplaceSignature(rtn,
            AFUNPTR(CAPI_message_send_w),
            IARG_PROTOTYPE, proto,
            IARG_FUNCARG_ENTRYPOINT_VALUE, 0,
            IARG_FUNCARG_ENTRYPOINT_VALUE, 1,
            IARG_FUNCARG_ENTRYPOINT_VALUE, 2,
            IARG_FUNCARG_ENTRYPOINT_VALUE, 3,
            IARG_END);
   }
   else if (rtn_name == "CAPI_message_receive_w")
   {
      PROTO proto = PROTO_Allocate(PIN_PARG(CAPI_return_t),
            CALLINGSTD_DEFAULT,
            "CAPI_message_receive_w",
            PIN_PARG(CAPI_endpoint_t),
            PIN_PARG(CAPI_endpoint_t),
            PIN_PARG(char*),
            PIN_PARG(int),
            PIN_PARG_END());

      RTN_ReplaceSignature(rtn,
            AFUNPTR(CAPI_message_receive_w),
            IARG_PROTOTYPE, proto,
            IARG_FUNCARG_ENTRYPOINT_VALUE, 0,
            IARG_FUNCARG_ENTRYPOINT_VALUE, 1,
            IARG_FUNCARG_ENTRYPOINT_VALUE, 2,
            IARG_FUNCARG_ENTRYPOINT_VALUE, 3,
            IARG_END);
   }
   else if (rtn_name == "CAPI_message_send_w_ex")
   {
      PROTO proto = PROTO_Allocate(PIN_PARG(CAPI_return_t),
            CALLINGSTD_DEFAULT,
            "CAPI_message_send_w_ex",
            PIN_PARG(CAPI_endpoint_t),
            PIN_PARG(CAPI_endpoint_t),
            PIN_PARG(char*),
            PIN_PARG(int),
            PIN_PARG(UInt32),
            PIN_PARG_END());

      RTN_ReplaceSignature(rtn,
            AFUNPTR(CAPI_message_send_w_ex),
            IARG_PROTOTYPE, proto,
            IARG_FUNCARG_ENTRYPOINT_VALUE, 0,
            IARG_FUNCARG_ENTRYPOINT_VALUE, 1,
            IARG_FUNCARG_ENTRYPOINT_VALUE, 2,
            IARG_FUNCARG_ENTRYPOINT_VALUE, 3,
            IARG_FUNCARG_ENTRYPOINT_VALUE, 4,
            IARG_END);
   }
   else if (rtn_name == "CAPI_message_receive_w_ex")
   {
      PROTO proto = PROTO_Allocate(PIN_PARG(CAPI_return_t),
            CALLINGSTD_DEFAULT,
            "CAPI_message_receive_w_ex",
            PIN_PARG(CAPI_endpoint_t),
            PIN_PARG(CAPI_endpoint_t),
            PIN_PARG(char*),
            PIN_PARG(int),
            PIN_PARG(UInt32),
            PIN_PARG_END());

      RTN_ReplaceSignature(rtn,
            AFUNPTR(CAPI_message_receive_w_ex),
            IARG_PROTOTYPE, proto,
            IARG_FUNCARG_ENTRYPOINT_VALUE, 0,
            IARG_FUNCARG_ENTRYPOINT_VALUE, 1,
            IARG_FUNCARG_ENTRYPOINT_VALUE, 2,
            IARG_FUNCARG_ENTRYPOINT_VALUE, 3,
            IARG_FUNCARG_ENTRYPOINT_VALUE, 4,
            IARG_END);
   }

   // os emulation
   else if (rtn_name == "get_nprocs")        RTN_Replace(rtn, AFUNPTR(emuGetNprocs));
   else if (rtn_name == "get_nprocs_conf")   RTN_Replace(rtn, AFUNPTR(emuGetNprocs));
   else if (rtn_name == "clock_gettime")     RTN_Replace(rtn, AFUNPTR(emuClockGettime));

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

carbon_thread_t emuCarbonSpawnThread(CONTEXT* context,
      thread_func_t thread_func, void* arg)
{
   LOG_PRINT("Entering emuCarbonSpawnThread(%p, %p)", thread_func, arg);

   core_id_t tid = Sim()->getThreadManager()->spawnThread(thread_func, arg);

   LOG_ASSERT_ERROR(pthread_create_func != NULL, "Could not find pthread_create");

   int ret;
   pthread_t* thread_ptr = new pthread_t;
   PIN_CallApplicationFunction(context, PIN_ThreadId(),
         CALLINGSTD_DEFAULT,
         pthread_create_func,
         PIN_PARG(int), &ret,
         PIN_PARG(pthread_t*), thread_ptr,
         PIN_PARG(pthread_attr_t*), NULL,
         PIN_PARG(void* (*)(void*)), thread_func,
         PIN_PARG(void*), arg,
         PIN_PARG_END());

   LOG_ASSERT_ERROR(ret == 0, "pthread_create() returned(%i)", ret);

   // FIXME: Figure out if we need to put a lock
   tid_to_thread_ptr_map.insert(std::pair<core_id_t, pthread_t>(tid, *thread_ptr));

   return tid;
}

void emuPthreadCreateBefore(THREADID thread_id, ADDRINT thread_ptr, void* (*thread_func)(void*), void* arg)
{
   // We have to do a loose match on pthread_create (it's sometimes called __pthread_create_2_1),
   // but that can cause recursion on this function. Detect this by keeping a count
   // and only act on the outer call.
   if (0 == localStore[thread_id].pthread_create.count++) {
      core_id_t tid = Sim()->getThreadManager()->spawnThread(thread_func, arg);

      LOG_ASSERT_ERROR(pthread_create_func != NULL, "Could not find pthread_create");

      localStore[thread_id].pthread_create.thread_ptr = thread_ptr;
      localStore[thread_id].pthread_create.tid = tid;
    }
}

void emuPthreadCreateAfter(THREADID thread_id)
{
   if (0 == --localStore[thread_id].pthread_create.count) {
      pthread_t* thread_ptr = (pthread_t*)localStore[thread_id].pthread_create.thread_ptr;
      core_id_t tid = localStore[thread_id].pthread_create.tid;
      tid_to_thread_ptr_map.insert(std::pair<core_id_t, pthread_t>(tid, *thread_ptr));
   }
}

void emuCarbonJoinThread(CONTEXT* context,
      carbon_thread_t tid)
{
   multimap<core_id_t, pthread_t>::iterator it;

   it = tid_to_thread_ptr_map.find(tid);
   LOG_ASSERT_ERROR(it != tid_to_thread_ptr_map.end(),
         "Cant find thread_ptr for tid(%i)", tid);

   pthread_t thread = it->second;

   LOG_PRINT("Starting emuCarbonJoinThread: thread(%p), tid(%i)", thread, tid);

   Sim()->getThreadManager()->joinThread(tid);

   tid_to_thread_ptr_map.erase(it);

   LOG_ASSERT_ERROR(pthread_join_func != NULL, "Could not find pthread_join");

   int ret;
   PIN_CallApplicationFunction(context, PIN_ThreadId(),
         CALLINGSTD_DEFAULT,
         pthread_join_func,
         PIN_PARG(int), &ret,
         PIN_PARG(pthread_t), thread,
         PIN_PARG(void**), NULL,
         PIN_PARG_END());

   LOG_ASSERT_ERROR(ret == 0, "pthread_join() returned(%i)", ret);

   LOG_PRINT("Finished emuCarbonJoinThread: thread(%p), tid(%i)", thread, tid);
}

void emuPthreadJoinBefore(pthread_t thread)
{
   core_id_t tid = INVALID_CORE_ID;

   multimap<core_id_t, pthread_t>::iterator it;
   for (it = tid_to_thread_ptr_map.begin(); it != tid_to_thread_ptr_map.end(); it++)
   {
      if (pthread_equal(it->second, thread) != 0)
      {
         tid = it->first;
         break;
      }
   }
   LOG_ASSERT_ERROR(tid != INVALID_CORE_ID, "Could not find core_id");

   LOG_PRINT("Joining Thread_ptr(%p), tid(%i)", &thread, tid);

   Sim()->getThreadManager()->joinThread(tid);

   tid_to_thread_ptr_map.erase(it);

   LOG_ASSERT_ERROR(pthread_join_func != NULL, "Could not find pthread_join");
}

IntPtr nullFunction()
{
   LOG_PRINT("In nullFunction()");
   return IntPtr(0);
}

void pthreadBefore(THREADID thread_id)
{
   Core* core = Sim()->getCoreManager()->getCurrentCore(thread_id);
   pthread_t_start[thread_id] = core->getPerformanceModel()->getElapsedTime();
   updateState(core, PthreadEmu::STATE_WAITING);
}

void pthreadAfter(THREADID thread_id, ADDRINT type_id, ADDRINT retval)
{
   Core* core = Sim()->getCoreManager()->getCurrentCore(thread_id);
   PthreadEmu::state_t new_state;
   if (pthread_functions[type_id].state_after == PthreadEmu::STATE_BY_RETURN)
      new_state = retval == EBUSY ? PthreadEmu::STATE_RUNNING : PthreadEmu::STATE_INREGION;
   else
      new_state = pthread_functions[type_id].state_after;
   updateState(core, new_state);
   pthreadCount(pthread_functions[type_id].function, core, core->getPerformanceModel()->getElapsedTime() - pthread_t_start[thread_id], SubsecondTime::Zero());
}

IntPtr emuGetNprocs()
{
   return Sim()->getConfig()->getOSEmuNprocs()
   ? Sim()->getConfig()->getOSEmuNprocs()
   : Sim()->getConfig()->getApplicationCores();
}

IntPtr emuClockGettime(clockid_t clk_id, struct timespec *tp)
{
   switch(clk_id)
   {
      case CLOCK_REALTIME:
      case CLOCK_MONOTONIC:
         // Return simulated time
         if (tp)
         {
            Core* core = Sim()->getCoreManager()->getCurrentCore();
            UInt64 time = core->getPerformanceModel()->getElapsedTime().getNS();

            tp->tv_sec = time / 1000000000;
            tp->tv_nsec = time % 1000000000;
         }
         return 0;
      default:
         // Unknown/non-emulated clock types (such as CLOCK_PROCESS_CPUTIME_ID/CLOCK_THREAD_CPUTIME_ID)
         return clock_gettime(clk_id, tp);
   }
}

}
