#include "routine_replace.h"
#include "pthread_emu.h"
#include "simulator.h"
#include "thread_manager.h"
#include "log.h"
#include "thread_support_private.h"
#include "sync_api.h"
#include "perf_counter_support.h"
#include "stats.h"
#include "logmem.h"
#include "magic_client.h"

#include <malloc.h>
#include <errno.h>
#include <stdlib.h>


// -------------------------------------
// Begin Memory redirection stuff
#include "config.h"
#include "simulator.h"
#include "core.h"
#include "core_manager.h"
#include "redirect_memory.h"
#include "thread_start.h"
#include "network.h"
#include "packet_type.h"
// End Memory redirection stuff
// --------------------------------------

// ---------------------------------------------------------
// Memory Redirection
//
// Routine replacement needs to work a little differently
// We can't use RTN_Replace or RTN_ReplaceSignature because
// the application executes out of a different memory space
// (and hence also uses a different stack) that Pin isn't aware of
// Our solution is to insert a call before the routine executes
// to a wrapper function that extracts the correct arguments from
// the user stack (including populating memory pointers with data
// from simulated memory) and then calls the "replacement functions"
// within the pintool. This wrapper also writes back results to user
// memory before returning control to the correct return address
// in user space
//
// --------------------------------------------------------------



bool replaceUserAPIFunction(RTN& rtn, string& name, INS& ins)
{
   AFUNPTR msg_ptr = NULL;

   // TODO: Check that the starting stack is located below the text segment
   // thread management
   if (name == "main") msg_ptr = AFUNPTR (replacementMain);
   else if (name == "CarbonGetThreadToSpawn") msg_ptr = AFUNPTR(replacementGetThreadToSpawn);
   else if (name == "CarbonThreadStart") msg_ptr = AFUNPTR (replacementThreadStartNull);
   else if (name == "CarbonThreadExit") msg_ptr = AFUNPTR (replacementThreadExitNull);
   else if (name == "CarbonGetCoreId") msg_ptr = AFUNPTR(replacementGetCoreId);
   else if (name == "CarbonDequeueThreadSpawnReq") msg_ptr = AFUNPTR (replacementDequeueThreadSpawnRequest);

   // PIN specific stack management
   else if (name == "CarbonPthreadAttrInitOtherAttr") msg_ptr = AFUNPTR(replacementPthreadAttrInitOtherAttr);

   // Carbon API

   else if (name == "CarbonStartSim") msg_ptr = AFUNPTR(replacementStartSimNull);
   else if (name == "CarbonStopSim") msg_ptr = AFUNPTR(replacementStopSim);
   else if (name == "CarbonSpawnThread") msg_ptr = AFUNPTR(replacementSpawnThread);
   else if (name == "CarbonJoinThread") msg_ptr = AFUNPTR(replacementJoinThread);

   // CAPI
   else if (name == "CAPI_Initialize") msg_ptr = AFUNPTR(replacement_CAPI_Initialize);
   else if (name == "CAPI_rank") msg_ptr = AFUNPTR(replacement_CAPI_rank);
   else if (name == "CAPI_message_send_w") msg_ptr = AFUNPTR(replacement_CAPI_message_send_w);
   else if (name == "CAPI_message_receive_w") msg_ptr = AFUNPTR(replacement_CAPI_message_receive_w);
   else if (name == "CAPI_message_send_w_ex") msg_ptr = AFUNPTR(replacement_CAPI_message_send_w_ex);
   else if (name == "CAPI_message_receive_w_ex") msg_ptr = AFUNPTR(replacement_CAPI_message_receive_w_ex);

   // synchronization
   else if (name == "CarbonMutexInit") msg_ptr = AFUNPTR(replacementMutexInit);
   else if (name == "CarbonMutexLock") msg_ptr = AFUNPTR(replacementMutexLock);
   else if (name == "CarbonMutexUnlock") msg_ptr = AFUNPTR(replacementMutexUnlock);
   else if (name == "CarbonCondInit") msg_ptr = AFUNPTR(replacementCondInit);
   else if (name == "CarbonCondWait") msg_ptr = AFUNPTR(replacementCondWait);
   else if (name == "CarbonCondSignal") msg_ptr = AFUNPTR(replacementCondSignal);
   else if (name == "CarbonCondBroadcast") msg_ptr = AFUNPTR(replacementCondBroadcast);
   else if (name == "CarbonBarrierInit") msg_ptr = AFUNPTR(replacementBarrierInit);
   else if (name == "CarbonBarrierWait") msg_ptr = AFUNPTR(replacementBarrierWait);

   // Resetting Cache Counters
   else if (name == "CarbonResetCacheCounters") msg_ptr = AFUNPTR(replacementResetCacheCounters);
   else if (name == "CarbonDisableCacheCounters") msg_ptr = AFUNPTR(replacementDisableCacheCounters);

   // os emulation
   else if (name == "get_nprocs") msg_ptr = AFUNPTR(replacementGetNprocs);
   else if (name == "get_nprocs_conf") msg_ptr = AFUNPTR(replacementGetNprocs);

   // pthread wrappers
   else if (name == "pthread_create") msg_ptr = AFUNPTR(replacementPthreadCreate);
   else if (name == "__pthread_create_2_1") msg_ptr = AFUNPTR(replacementPthreadCreate);
   else if (name.find("pthread_self") != String::npos) msg_ptr = AFUNPTR(replacementPthreadSelf);
   else if (name.find("pthread_join") != String::npos) msg_ptr = AFUNPTR(replacementPthreadJoin);
   else if (name.find("pthread_mutex_init") != String::npos) msg_ptr = AFUNPTR(replacementPthreadMutexInit);
   else if (name.find("pthread_mutex_lock") != String::npos) msg_ptr = AFUNPTR(replacementPthreadMutexLock);
   else if (name.find("pthread_mutex_trylock") != String::npos) msg_ptr = AFUNPTR(replacementPthreadMutexTrylock);
   else if (name.find("pthread_mutex_unlock") != String::npos) msg_ptr = AFUNPTR(replacementPthreadMutexUnlock);
   else if (name.find("pthread_mutex_destroy") != String::npos) msg_ptr = AFUNPTR(replacementPthreadMutexDestroy);
   else if (name.find("pthread_cond_init") != String::npos) msg_ptr = AFUNPTR(replacementPthreadCondInit);
   else if (name.find("pthread_cond_wait") != String::npos) msg_ptr = AFUNPTR(replacementPthreadCondWait);
   else if (name.find("pthread_cond_signal") != String::npos) msg_ptr = AFUNPTR(replacementPthreadCondSignal);
   else if (name.find("pthread_cond_broadcast") != String::npos) msg_ptr = AFUNPTR(replacementPthreadCondBroadcast);
   else if (name.find("pthread_cond_destroy") != String::npos) msg_ptr = AFUNPTR(replacementPthreadCondDestroy);
   else if (name.find("pthread_barrier_init") != String::npos) msg_ptr = AFUNPTR(replacementPthreadBarrierInit);
   else if (name.find("pthread_barrier_wait") != String::npos) msg_ptr = AFUNPTR(replacementPthreadBarrierWait);
   else if (name.find("pthread_barrier_destroy") != String::npos) msg_ptr = AFUNPTR(replacementPthreadBarrierDestroy);
   else if (name.find("pthread_exit") != String::npos) msg_ptr = AFUNPTR(replacementPthreadExitNull);

   // turn off performance modeling after main()
   if (name == "main")
   {
      RTN_Open (rtn);
      RTN_InsertCall (rtn, IPOINT_AFTER,
                      AFUNPTR(afterMain),
                      IARG_CONTEXT,
                      IARG_END);
      RTN_Close (rtn);
   }
   // or, when the application explicitly calls exit(), do it there
   if (name == "exit")
   {
      RTN_Open (rtn);
      RTN_InsertCall (rtn, IPOINT_BEFORE,
                      AFUNPTR(afterMain),
                      IARG_CONTEXT,
                      IARG_END);
      RTN_Close (rtn);
   }

   // do replacement
   if (msg_ptr != NULL)
   {
      INS_InsertCall (ins, IPOINT_BEFORE,
            msg_ptr,
            IARG_CONTEXT,
            IARG_END);

      return true;
   }
   else
   {
      return false;
   }
}

void replacementMain (CONTEXT *ctxt)
{
   LOG_PRINT ("In replacementMain");

   if (Sim()->getConfig()->getCurrentProcessNum() == 0)
   {
      LOG_PRINT("ReplaceMain start");

      Core *core = Sim()->getCoreManager()->getCurrentCore();
      UInt32 num_processes = Sim()->getConfig()->getProcessCount();
      for (UInt32 i = 1; i < num_processes; i++)
      {
         // FIXME:
         // This whole process should probably happen through the MCP
         core->getNetwork()->netSend (Sim()->getConfig()->getThreadSpawnerCoreNum (i), SYSTEM_INITIALIZATION_NOTIFY, NULL, 0);

         // main thread clock is not affected by start-up time of other processes
         core->getNetwork()->netRecv (Sim()->getConfig()->getThreadSpawnerCoreNum (i), SYSTEM_INITIALIZATION_ACK);
      }

      for (UInt32 i = 1; i < num_processes; i++)
      {
         core->getNetwork()->netSend (Sim()->getConfig()->getThreadSpawnerCoreNum (i), SYSTEM_INITIALIZATION_FINI, NULL, 0);
      }

      spawnThreadSpawner(ctxt);

      if (! Sim()->getConfig()->useMagic())
         enablePerformanceGlobal();

      LOG_PRINT("ReplaceMain end");

      return;
   }
   else
   {
      // FIXME:
      // This whole process should probably happen through the MCP
      Core *core = Sim()->getCoreManager()->getCurrentCore();
      core->getNetwork()->netSend (Sim()->getConfig()->getMainThreadCoreNum(), SYSTEM_INITIALIZATION_ACK, NULL, 0);
      core->getNetwork()->netRecv (Sim()->getConfig()->getMainThreadCoreNum(), SYSTEM_INITIALIZATION_FINI);

      int res;
      ADDRINT reg_eip = PIN_GetContextReg (ctxt, REG_INST_PTR);

      PIN_LockClient();

      AFUNPTR thread_spawner;
      IMG img = IMG_FindByAddress(reg_eip);
      RTN rtn = RTN_FindByName(img, "CarbonThreadSpawner");
      thread_spawner = RTN_Funptr(rtn);

      PIN_UnlockClient();

      PIN_CallApplicationFunction (ctxt,
            PIN_ThreadId(),
            CALLINGSTD_DEFAULT,
            thread_spawner,
            PIN_PARG(int), &res,
            PIN_PARG(void*), NULL,
            PIN_PARG_END());

      Sim()->getThreadManager()->onThreadExit();

      while (!Sim()->finished())
         sched_yield();

      Simulator::release();

      exit (0);
   }
}

void afterMain (CONTEXT *ctxt)
{
   if (! Sim()->getConfig()->useMagic())
      disablePerformanceGlobal();
}

void replacementGetThreadToSpawn (CONTEXT *ctxt)
{
   Core *core = Sim()->getCoreManager()->getCurrentCore();
   assert (core != NULL);

   ThreadSpawnRequest *req;
   ThreadSpawnRequest req_buf;
   initialize_replacement_args (ctxt,
         IARG_PTR, &req,
         IARG_END);

   // Preserve REG_GAX across function call with
   // void return type
   ADDRINT ret_val = PIN_GetContextReg (ctxt, REG_GAX);

   Sim()->getThreadManager()->getThreadToSpawn(&req_buf);

   core->accessMemory(Core::NONE, Core::WRITE, (IntPtr) req, (char*) &req_buf, sizeof (ThreadSpawnRequest));

   retFromReplacedRtn (ctxt, ret_val);
}

void replacementThreadStartNull (CONTEXT *ctxt)
{
   ADDRINT ret_val = PIN_GetContextReg (ctxt, REG_GAX);
   retFromReplacedRtn (ctxt, ret_val);
}

void replacementThreadExitNull (CONTEXT *ctxt)
{
   ADDRINT ret_val = PIN_GetContextReg (ctxt, REG_GAX);
   retFromReplacedRtn (ctxt, ret_val);
}

void replacementGetCoreId (CONTEXT *ctxt)
{
   Core *core = Sim()->getCoreManager()->getCurrentCore();
   assert (core);
   retFromReplacedRtn (ctxt, core->getId());
}

void replacementDequeueThreadSpawnRequest (CONTEXT *ctxt)
{
   Core *core = Sim()->getCoreManager()->getCurrentCore();
   assert (core);

   ThreadSpawnRequest *thread_req;
   ThreadSpawnRequest thread_req_buf;

   initialize_replacement_args (ctxt,
         IARG_PTR, &thread_req,
         IARG_END);
   ADDRINT ret_val = PIN_GetContextReg (ctxt, REG_GAX);

   Sim()->getThreadManager()->dequeueThreadSpawnReq(&thread_req_buf);

   core->accessMemory (Core::NONE, Core::WRITE, (IntPtr) thread_req, (char*) &thread_req_buf, sizeof (ThreadSpawnRequest));

   retFromReplacedRtn (ctxt, ret_val);
}


// PIN specific stack management
void replacementPthreadAttrInitOtherAttr(CONTEXT *ctxt)
{
   Core *core = Sim()->getCoreManager()->getCurrentCore();
   assert(core != NULL);

   pthread_attr_t *attr;
   pthread_attr_t attr_buf;

   initialize_replacement_args(ctxt,
         IARG_PTR, &attr,
         IARG_END);

   core->accessMemory (Core::NONE, Core::READ, (ADDRINT) attr, (char*) &attr_buf, sizeof (pthread_attr_t));

   ADDRINT ret_val = PIN_GetContextReg(ctxt, REG_GAX);

   SimPthreadAttrInitOtherAttr(&attr_buf);

   core->accessMemory (Core::NONE, Core::WRITE, (ADDRINT) attr, (char*) &attr_buf, sizeof (pthread_attr_t));

   retFromReplacedRtn(ctxt, ret_val);
}


void replacementStartSimNull (CONTEXT *ctxt)
{
   ADDRINT ret_val = 0;
   retFromReplacedRtn (ctxt, ret_val);
}

void replacementStopSim (CONTEXT *ctxt)
{
   ADDRINT ret_val = PIN_GetContextReg (ctxt, REG_GAX);
   retFromReplacedRtn (ctxt, ret_val);
}

void replacementSpawnThread (CONTEXT *ctxt)
{
   thread_func_t func;
   void *arg;

   initialize_replacement_args (ctxt,
         IARG_PTR, &func,
         IARG_PTR, &arg,
         IARG_END);

   LOG_PRINT("Calling SimSpawnThread");
   ADDRINT ret_val = (ADDRINT) Sim()->getThreadManager()->spawnThread(func, arg);

   retFromReplacedRtn (ctxt, ret_val);
}

void replacementJoinThread (CONTEXT *ctxt)
{
   ADDRINT tid;

   initialize_replacement_args (ctxt,
         IARG_ADDRINT, &tid,
         IARG_END);

   ADDRINT ret_val = PIN_GetContextReg (ctxt, REG_GAX);

   Sim()->getThreadManager()->joinThread((core_id_t) tid);

   retFromReplacedRtn (ctxt, ret_val);
}

void replacement_CAPI_Initialize (CONTEXT *ctxt)
{
   // Only the user-threads (all of which are cores) call
   // the CAPI communication API functions
   __attribute(__unused__) Core *core = Sim()->getCoreManager()->getCurrentCore();
   assert (core);

   int comm_id;
   initialize_replacement_args (ctxt,
         IARG_UINT32, &comm_id,
         IARG_END);

   ADDRINT ret_val = PIN_GetContextReg (ctxt, REG_GAX);

   CAPI_Initialize (comm_id);

   retFromReplacedRtn (ctxt, ret_val);
}

void replacement_CAPI_rank (CONTEXT *ctxt)
{
   // Only the user-threads (all of which are cores) call
   // the CAPI communication API functions
   Core *core = Sim()->getCoreManager()->getCurrentCore();
   assert (core);

   int *core_id;
   int core_id_buf;
   CAPI_return_t ret_val;

   initialize_replacement_args (ctxt,
         IARG_PTR, &core_id,
         IARG_END);

   ret_val = CAPI_rank (&core_id_buf);

   core->accessMemory (Core::NONE, Core::WRITE, (ADDRINT) core_id, (char*) &core_id_buf, sizeof (int));

   retFromReplacedRtn (ctxt, ret_val);
}

void replacement_CAPI_message_send_w (CONTEXT *ctxt)
{
   // Only the user-threads (all of which are cores) call
   // the CAPI communication API functions
   Core *core = Sim()->getCoreManager()->getCurrentCore();
   assert (core);

   CAPI_endpoint_t sender;
   CAPI_endpoint_t receiver;
   char *buffer;
   int size;
   CAPI_return_t ret_val = 0;

   initialize_replacement_args (ctxt,
         IARG_UINT32, &sender,
         IARG_UINT32, &receiver,
         IARG_PTR, &buffer,
         IARG_UINT32, &size,
         IARG_END);

   char *buf = new char [size];
   core->accessMemory (Core::NONE, Core::READ, (ADDRINT) buffer, buf, size);
   ret_val = CAPI_message_send_w (sender, receiver, buf, size);

   delete [] buf;
   retFromReplacedRtn (ctxt, ret_val);
}

void replacement_CAPI_message_send_w_ex (CONTEXT *ctxt)
{
   // Only the user-threads (all of which are cores) call
   // the CAPI communication API functions
   Core *core = Sim()->getCoreManager()->getCurrentCore();
   assert (core);

   CAPI_endpoint_t sender;
   CAPI_endpoint_t receiver;
   char *buffer;
   int size;
   carbon_network_t net_type;
   CAPI_return_t ret_val = 0;

   initialize_replacement_args (ctxt,
         IARG_UINT32, &sender,
         IARG_UINT32, &receiver,
         IARG_PTR, &buffer,
         IARG_UINT32, &size,
         IARG_UINT32, &net_type,
         IARG_END);

   char *buf = new char [size];
   core->accessMemory (Core::NONE, Core::READ, (ADDRINT) buffer, buf, size);
   ret_val = CAPI_message_send_w_ex (sender, receiver, buf, size, net_type);

   delete [] buf;
   retFromReplacedRtn (ctxt, ret_val);
}

void replacement_CAPI_message_receive_w (CONTEXT *ctxt)
{
   // Only the user-threads (all of which are cores) call
   // the CAPI communication API functions
   Core *core = Sim()->getCoreManager()->getCurrentCore();
   assert (core);

   CAPI_endpoint_t sender;
   CAPI_endpoint_t receiver;
   char *buffer;
   int size;
   CAPI_return_t ret_val = 0;

   initialize_replacement_args (ctxt,
         IARG_UINT32, &sender,
         IARG_UINT32, &receiver,
         IARG_PTR, &buffer,
         IARG_UINT32, &size,
         IARG_END);

   char *buf = new char [size];
   ret_val = CAPI_message_receive_w (sender, receiver, buf, size);
   core->accessMemory (Core::NONE, Core::WRITE, (ADDRINT) buffer, buf, size);

   delete [] buf;
   retFromReplacedRtn (ctxt, ret_val);
}

void replacement_CAPI_message_receive_w_ex (CONTEXT *ctxt)
{
   // Only the user-threads (all of which are cores) call
   // the CAPI communication API functions
   Core *core = Sim()->getCoreManager()->getCurrentCore();
   assert (core);

   CAPI_endpoint_t sender;
   CAPI_endpoint_t receiver;
   char *buffer;
   int size;
   carbon_network_t net_type;
   CAPI_return_t ret_val = 0;

   initialize_replacement_args (ctxt,
         IARG_UINT32, &sender,
         IARG_UINT32, &receiver,
         IARG_PTR, &buffer,
         IARG_UINT32, &size,
         IARG_UINT32, &net_type,
         IARG_END);

   char *buf = new char [size];
   ret_val = CAPI_message_receive_w_ex (sender, receiver, buf, size, net_type);
   core->accessMemory (Core::NONE, Core::WRITE, (ADDRINT) buffer, buf, size);

   delete [] buf;
   retFromReplacedRtn (ctxt, ret_val);
}


void replacementMutexInit (CONTEXT *ctxt)
{
   // Only the user-threads (all of which are cores) call
   // the Carbon synch API functions
   Core *core = Sim()->getCoreManager()->getCurrentCore();
   assert (core);

   carbon_mutex_t *mux;
   initialize_replacement_args (ctxt,
         IARG_PTR, &mux,
         IARG_END);

   carbon_mutex_t mux_buf;
   ADDRINT ret_val = PIN_GetContextReg (ctxt, REG_GAX);

   core->accessMemory (Core::NONE, Core::READ, (ADDRINT) mux, (char*) &mux_buf, sizeof (mux_buf));
   CarbonMutexInit (&mux_buf);
   core->accessMemory (Core::NONE, Core::WRITE, (ADDRINT) mux, (char*) &mux_buf, sizeof (mux_buf));

   retFromReplacedRtn (ctxt, ret_val);
}

void replacementMutexLock (CONTEXT *ctxt)
{
   // Only the user-threads (all of which are cores) call
   // the Carbon synch API functions
   Core *core = Sim()->getCoreManager()->getCurrentCore();
   assert (core);

   carbon_mutex_t *mux;
   initialize_replacement_args (ctxt,
         IARG_PTR, &mux,
         IARG_END);

   carbon_mutex_t mux_buf;
   ADDRINT ret_val = PIN_GetContextReg (ctxt, REG_GAX);

   core->accessMemory (Core::NONE, Core::READ, (ADDRINT) mux, (char*) &mux_buf, sizeof (mux_buf));
   CarbonMutexLock (&mux_buf);

   retFromReplacedRtn (ctxt, ret_val);
}

void replacementMutexUnlock (CONTEXT *ctxt)
{
   // Only the user-threads (all of which are cores) call
   // the Carbon synch API functions
   Core *core = Sim()->getCoreManager()->getCurrentCore();
   assert (core);

   carbon_mutex_t *mux;
   initialize_replacement_args (ctxt,
         IARG_PTR, &mux,
         IARG_END);

   carbon_mutex_t mux_buf;
   ADDRINT ret_val = PIN_GetContextReg (ctxt, REG_GAX);

   core->accessMemory (Core::NONE, Core::READ, (ADDRINT) mux, (char*) &mux_buf, sizeof (mux_buf));
   CarbonMutexUnlock (&mux_buf);

   retFromReplacedRtn (ctxt, ret_val);
}

void replacementCondInit (CONTEXT *ctxt)
{
   // Only the user-threads (all of which are cores) call
   // the Carbon synch API functions
   Core *core = Sim()->getCoreManager()->getCurrentCore();
   assert (core);

   carbon_cond_t *cond;
   initialize_replacement_args (ctxt,
         IARG_PTR, &cond,
         IARG_END);

   carbon_cond_t cond_buf;
   ADDRINT ret_val = PIN_GetContextReg (ctxt, REG_GAX);

   core->accessMemory (Core::NONE, Core::READ, (ADDRINT) cond, (char*) &cond_buf, sizeof (cond_buf));
   CarbonCondInit (&cond_buf);
   core->accessMemory (Core::NONE, Core::WRITE, (ADDRINT) cond, (char*) &cond_buf, sizeof (cond_buf));

   retFromReplacedRtn (ctxt, ret_val);
}

void replacementCondWait (CONTEXT *ctxt)
{
   // Only the user-threads (all of which are cores) call
   // the Carbon synch API functions
   Core *core = Sim()->getCoreManager()->getCurrentCore();
   assert (core);

   carbon_cond_t *cond;
   carbon_mutex_t *mux;
   initialize_replacement_args (ctxt,
         IARG_PTR, &cond,
         IARG_PTR, &mux,
         IARG_END);

   carbon_cond_t cond_buf;
   carbon_mutex_t mux_buf;
   ADDRINT ret_val = PIN_GetContextReg (ctxt, REG_GAX);

   core->accessMemory (Core::NONE, Core::READ, (ADDRINT) cond, (char*) &cond_buf, sizeof (cond_buf));
   core->accessMemory (Core::NONE, Core::READ, (ADDRINT) mux, (char*) &mux_buf, sizeof (mux_buf));
   CarbonCondWait (&cond_buf, &mux_buf);

   retFromReplacedRtn (ctxt, ret_val);
}

void replacementCondSignal (CONTEXT *ctxt)
{
   // Only the user-threads (all of which are cores) call
   // the Carbon synch API functions
   Core *core = Sim()->getCoreManager()->getCurrentCore();
   assert (core);

   carbon_cond_t *cond;
   initialize_replacement_args (ctxt,
         IARG_PTR, &cond,
         IARG_END);

   carbon_cond_t cond_buf;
   ADDRINT ret_val = PIN_GetContextReg (ctxt, REG_GAX);

   core->accessMemory (Core::NONE, Core::READ, (ADDRINT) cond, (char*) &cond_buf, sizeof (cond_buf));
   CarbonCondSignal (&cond_buf);

   retFromReplacedRtn (ctxt, ret_val);
}

void replacementCondBroadcast (CONTEXT *ctxt)
{
   carbon_cond_t *cond;
   initialize_replacement_args (ctxt,
         IARG_PTR, &cond,
         IARG_END);

   carbon_cond_t cond_buf;
   ADDRINT ret_val = PIN_GetContextReg (ctxt, REG_GAX);

   Core *core = Sim()->getCoreManager()->getCurrentCore();
   assert (core);
   core->accessMemory (Core::NONE, Core::READ, (ADDRINT) cond, (char*) &cond_buf, sizeof (cond_buf));
   CarbonCondBroadcast (&cond_buf);

   retFromReplacedRtn (ctxt, ret_val);
}

void replacementBarrierInit (CONTEXT *ctxt)
{
   carbon_barrier_t *barrier;
   UInt32 count;
   initialize_replacement_args (ctxt,
         IARG_PTR, &barrier,
         IARG_UINT32, &count,
         IARG_END);

   carbon_barrier_t barrier_buf;
   ADDRINT ret_val = PIN_GetContextReg (ctxt, REG_GAX);

   Core *core = Sim()->getCoreManager()->getCurrentCore();
   assert (core);
   core->accessMemory (Core::NONE, Core::READ, (ADDRINT) barrier, (char*) &barrier_buf, sizeof (barrier_buf));
   CarbonBarrierInit (&barrier_buf, count);
   core->accessMemory (Core::NONE, Core::WRITE, (ADDRINT) barrier, (char*) &barrier_buf, sizeof (barrier_buf));

   retFromReplacedRtn (ctxt, ret_val);
}

void replacementBarrierWait (CONTEXT *ctxt)
{
   carbon_barrier_t *barrier;
   initialize_replacement_args (ctxt,
         IARG_ADDRINT, &barrier,
         IARG_END);

   carbon_barrier_t barrier_buf;
   ADDRINT ret_val = PIN_GetContextReg (ctxt, REG_GAX);

   Core *core = Sim()->getCoreManager()->getCurrentCore();
   assert (core);
   core->accessMemory (Core::NONE, Core::READ, (ADDRINT) barrier, (char*) &barrier_buf, sizeof (barrier_buf));
   CarbonBarrierWait (&barrier_buf);

   retFromReplacedRtn (ctxt, ret_val);
}

//assumption: the pthread_create from the ThreadSpawner is the first pthread_create() we encounter, and we need to let it fall through
bool pthread_create_first_time = true;

void replacementPthreadCreate (CONTEXT *ctxt)
{
   core_id_t current_core_id =
                        Sim()->getCoreManager()->getCurrentCoreID();
   core_id_t current_thread_spawner_id =
                        Sim()->getConfig()->getCurrentThreadSpawnerCoreNum();

   if ( pthread_create_first_time || current_core_id == current_thread_spawner_id )
   {
      //let the pthread_create call fall through
      pthread_create_first_time = false;
   }
   else
   {
      pthread_t *thread_id;
      pthread_attr_t *attributes;
      thread_func_t func;
      void *func_arg;

      initialize_replacement_args (ctxt,
            IARG_PTR, &thread_id,
            IARG_PTR, &attributes,
            IARG_PTR, &func,
            IARG_PTR, &func_arg,
            IARG_END);

      //TODO: add support for different attributes and throw warnings for unsupported attrs

      if (attributes != NULL)
      {
         char sum = 0;
         for(int i = 0; i < __SIZEOF_PTHREAD_ATTR_T; ++i)
            sum |= attributes->__size[i];
         if (sum)
            fprintf(stdout, "Warning: pthread_create() is using unsupported attributes.\n");
      }

      SInt32 new_thread_id = Sim()->getThreadManager()->spawnThread(func, func_arg);

      Core *core = Sim()->getCoreManager()->getCurrentCore();
      assert (core);

      core->accessMemory(Core::NONE, Core::WRITE, (IntPtr) thread_id, (char*) &new_thread_id, sizeof (pthread_t));

      //pthread_create() expects a return value of 0 on success
      ADDRINT ret_val = 0;
      retFromReplacedRtn (ctxt, ret_val);
   }
}

void replacementPthreadSelf (CONTEXT *ctxt)
{
   // Should return a pthread_t struct, but we don't have that in this context
   // Can mess up programs that use this to get to their TLS
   Core *core = Sim()->getCoreManager()->getCurrentCore();
   assert (core);
   retFromReplacedRtn (ctxt, core->getId());
}

void replacementPthreadJoin (CONTEXT *ctxt)
{
   pthread_t thread_id;
   void** return_value;

   initialize_replacement_args (ctxt,
         IARG_PTR, &thread_id,
         IARG_PTR, &return_value,
         IARG_END);

   LOG_ASSERT_WARNING (return_value == NULL, "pthread_join() is expecting a return value \
         to be passed through value_ptr input, which is unsupported");

   Sim()->getThreadManager()->joinThread((core_id_t) thread_id);

   //pthread_join() expects a return value of 0 on success
   ADDRINT ret_val = 0;
   retFromReplacedRtn (ctxt, ret_val);
}

void replacementPthreadExitNull (CONTEXT *ctxt)
{
   ADDRINT ret_val = PIN_GetContextReg (ctxt, REG_GAX);
   retFromReplacedRtn (ctxt, ret_val);
}

void replacementPthreadMutexInit (CONTEXT *ctxt)
{
   pthread_mutex_t *mux;
   pthread_mutexattr_t *attributes, _attributes;

   initialize_replacement_args (ctxt,
         IARG_PTR, &mux,
         IARG_PTR, &attributes,
         IARG_END);

   if (attributes != NULL)
   {
      Core *core = Sim()->getCoreManager()->getCurrentCore();
      assert (core);
      core->accessMemory(Core::NONE, Core::READ, (IntPtr) attributes, (char *) &_attributes, sizeof (pthread_mutexattr_t));
   }

   ADDRINT res = PthreadEmu::MutexInit(mux, attributes ? &_attributes : NULL);

   retFromReplacedRtn(ctxt, res);
}

void replacementPthreadMutexLock (CONTEXT *ctxt)
{
   pthread_mutex_t *mux;

   initialize_replacement_args (ctxt,
         IARG_PTR, &mux,
         IARG_END);

   ADDRINT res = PthreadEmu::MutexLock(mux);

   retFromReplacedRtn (ctxt, res);
}

void replacementPthreadMutexTrylock (CONTEXT *ctxt)
{
   pthread_mutex_t *mux;

   initialize_replacement_args (ctxt,
         IARG_PTR, &mux,
         IARG_END);

   ADDRINT res = PthreadEmu::MutexTrylock(mux);

   retFromReplacedRtn (ctxt, res);
}

void replacementPthreadMutexUnlock (CONTEXT *ctxt)
{
   pthread_mutex_t *mux;

   initialize_replacement_args (ctxt,
         IARG_PTR, &mux,
         IARG_END);

   ADDRINT res = PthreadEmu::MutexUnlock(mux);

   retFromReplacedRtn (ctxt, res);
}

void replacementPthreadMutexDestroy(CONTEXT *ctxt) {
   retFromReplacedRtn (ctxt, 0);
}

void replacementPthreadCondInit (CONTEXT *ctxt)
{
   pthread_cond_t *cond;
   pthread_condattr_t *attributes, _attributes;

   initialize_replacement_args (ctxt,
         IARG_PTR, &cond,
         IARG_PTR, &attributes,
         IARG_END);

   if (attributes != NULL)
   {
      Core *core = Sim()->getCoreManager()->getCurrentCore();
      assert (core);
      core->accessMemory(Core::NONE, Core::READ, (IntPtr) attributes, (char *) &_attributes, sizeof (pthread_condattr_t));
   }

   ADDRINT res = PthreadEmu::CondInit(cond, attributes ? &_attributes : NULL);

   retFromReplacedRtn (ctxt, res);
}

void replacementPthreadCondWait (CONTEXT *ctxt)
{
   pthread_cond_t *cond;
   pthread_mutex_t *mutex;

   initialize_replacement_args (ctxt,
         IARG_PTR, &cond,
         IARG_PTR, &mutex,
         IARG_END);

   ADDRINT res = PthreadEmu::CondWait(cond, mutex);

   retFromReplacedRtn (ctxt, res);
}

void replacementPthreadCondSignal (CONTEXT *ctxt)
{
   pthread_cond_t *cond;

   initialize_replacement_args (ctxt,
         IARG_PTR, &cond,
         IARG_END);

   ADDRINT res = PthreadEmu::CondSignal(cond);

   retFromReplacedRtn (ctxt, res);
}

void replacementPthreadCondBroadcast (CONTEXT *ctxt)
{
   pthread_cond_t *cond;

   initialize_replacement_args (ctxt,
         IARG_PTR, &cond,
         IARG_END);

   ADDRINT res = PthreadEmu::CondBroadcast(cond);

   retFromReplacedRtn (ctxt, res);
}

void replacementPthreadCondDestroy(CONTEXT *ctxt)
{
   retFromReplacedRtn (ctxt, 0);
}

void replacementPthreadBarrierInit (CONTEXT *ctxt)
{
   pthread_barrier_t *barrier;
   pthread_barrierattr_t *attributes, _attributes;
   UInt32 count;

   initialize_replacement_args (ctxt,
         IARG_PTR, &barrier,
         IARG_PTR, &attributes,
         IARG_UINT32, &count,
         IARG_END);

   if (attributes != NULL)
   {
      Core *core = Sim()->getCoreManager()->getCurrentCore();
      assert (core);
      core->accessMemory(Core::NONE, Core::READ, (IntPtr) attributes, (char *) &_attributes, sizeof (pthread_barrierattr_t));
   }

   ADDRINT res = PthreadEmu::BarrierInit(barrier, attributes ? &_attributes : NULL, count);

   retFromReplacedRtn (ctxt, res);
}

void replacementPthreadBarrierWait (CONTEXT *ctxt)
{
   pthread_barrier_t *barrier;

   initialize_replacement_args (ctxt,
         IARG_PTR, &barrier,
         IARG_END);

   ADDRINT res = PthreadEmu::BarrierWait(barrier);

   retFromReplacedRtn (ctxt, res);
}

void replacementPthreadBarrierDestroy(CONTEXT *ctxt)
{
   retFromReplacedRtn (ctxt, 0);
}

void replacementResetCacheCounters (CONTEXT *ctxt)
{
   CarbonResetCacheCounters();

   ADDRINT ret_val = PIN_GetContextReg (ctxt, REG_GAX);
   retFromReplacedRtn (ctxt, ret_val);
}

void replacementDisableCacheCounters (CONTEXT *ctxt)
{
   CarbonDisableCacheCounters();

   ADDRINT ret_val = PIN_GetContextReg (ctxt, REG_GAX);
   retFromReplacedRtn (ctxt, ret_val);
}

void replacementGetNprocs (CONTEXT *ctxt)
{
   ADDRINT ret_val = Sim()->getConfig()->getApplicationCores();
   retFromReplacedRtn (ctxt, ret_val);
}

void initialize_replacement_args (CONTEXT *ctxt, ...)
{
#ifdef TARGET_IA32
   va_list vl;
   va_start (vl, ctxt);
   int type;
   ADDRINT addr;
   ADDRINT ptr;
   ADDRINT buffer;
   unsigned int count = 0;
   Core *core = Sim()->getCoreManager()->getCurrentCore();
   assert (core);

   do
   {
      type = va_arg (vl, int);
      addr = PIN_GetContextReg (ctxt, REG_STACK_PTR) + ((count + 1) * sizeof (ADDRINT));

      core->accessMemory (Core::NONE, Core::READ, addr, (char*) &buffer, sizeof (ADDRINT));
      switch (type)
      {
         case IARG_ADDRINT:
            ptr = va_arg (vl, ADDRINT);
            * ((ADDRINT*) ptr) = buffer;
            count++;
            break;

         case IARG_PTR:
            ptr = va_arg (vl, ADDRINT);
            * ((ADDRINT*) ptr) = buffer;
            count++;
            break;

         case IARG_UINT32:
            ptr = va_arg (vl, ADDRINT);
            * ((UInt32*) ptr) = (UInt32) buffer;
            count++;
            break;

         case IARG_END:
            break;

         default:
            assert (false);
            break;
      }
   } while (type != IARG_END);
#endif

#ifdef TARGET_X86_64
   va_list vl;
   va_start (vl, ctxt);
   int type;
   ADDRINT ptr;
   ADDRINT val;
   unsigned int count = 0;
   REG reg_sequence[] = {REG_GDI, REG_GSI, REG_GDX, REG_GCX, REG_R8, REG_R9};

   do
   {
      LOG_ASSERT_ERROR (count < 6, "Don't support more than 6 function call arguments for replaced functions");

      type = va_arg (vl, int);
      val = PIN_GetContextReg (ctxt, reg_sequence[count]);
      LOG_PRINT("function args(%i) -> 0x%x", count, (IntPtr) val);

      switch (type)
      {
         case IARG_ADDRINT:
            ptr = va_arg (vl, ADDRINT);
            * ((ADDRINT*) ptr) = val;
            count++;
            break;

         case IARG_PTR:
            ptr = va_arg (vl, ADDRINT);
            * ((ADDRINT*) ptr) = val;
            count++;
            break;

         case IARG_UINT32:
            ptr = va_arg (vl, ADDRINT);
            * ((UInt32*) ptr) = (UInt32) val;
            count++;
            break;

#if PIN_USES_IARG_LAST
         case IARG_FILE_NAME:
            ptr = va_arg (vl, ADDRINT);
            break;
         case IARG_LINE_NO:
            ptr = va_arg (vl, UINT32);
            break;
         case IARG_LAST:
            break;
#else
         case IARG_END:
            break;
#endif

         default:
            assert (false);
            break;
      }
#if PIN_USES_IARG_LAST
   } while (type != IARG_LAST);
#else
   } while (type != IARG_END);
#endif
#endif
}

void retFromReplacedRtn (CONTEXT *ctxt, ADDRINT ret_val)
{
   ADDRINT esp = PIN_GetContextReg (ctxt, REG_STACK_PTR);
   ADDRINT next_ip = emuRet (0, &esp, 0, sizeof (ADDRINT), Core::MEM_MODELED_NONE);

   PIN_SetContextReg (ctxt, REG_GAX, ret_val);
   PIN_SetContextReg (ctxt, REG_STACK_PTR, esp);
   PIN_SetContextReg (ctxt, REG_INST_PTR, next_ip);

   PIN_ExecuteAt (ctxt);
}

void setupCarbonSpawnThreadSpawnerStack (CONTEXT *ctx)
{
   // FIXME:
   // This will clearly need to change somewhat in the multi-process case
   // We can go back to our original scheme of having the "main" thread
   // on processes other than 0 execute the thread spawner, in which case
   // this will probably just work as is

   ADDRINT esp = PIN_GetContextReg (ctx, REG_STACK_PTR);
   ADDRINT ret_ip = * (ADDRINT*) esp;

   Core *core = Sim()->getCoreManager()->getCurrentCore();
   assert (core);

   core->accessMemory(Core::NONE, Core::WRITE, (IntPtr) esp, (char*) &ret_ip, sizeof (ADDRINT));
}

void setupCarbonThreadSpawnerStack (CONTEXT *ctx)
{
   if (Sim()->getConfig()->getCurrentProcessNum() == 0)
      return;

   ADDRINT esp = PIN_GetContextReg (ctx, REG_STACK_PTR);
   ADDRINT ret_ip = * (ADDRINT*) esp;
   ADDRINT p = * (ADDRINT*) (esp + sizeof (ADDRINT));

   Core *core = Sim()->getCoreManager()->getCurrentCore();
   assert (core);

   core->accessMemory (Core::NONE, Core::WRITE, (IntPtr) esp, (char*) &ret_ip, sizeof (ADDRINT));
   core->accessMemory (Core::NONE, Core::WRITE, (IntPtr) (esp + sizeof (ADDRINT)), (char*) &p, sizeof (ADDRINT));
}


