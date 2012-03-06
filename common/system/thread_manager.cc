#include <sys/syscall.h>

#include "thread_manager.h"
#include "core_manager.h"
#include "performance_model.h"
#include "hooks_manager.h"
#include "config.h"
#include "log.h"
#include "transport.h"
#include "simulator.h"
#include "mcp.h"
#include "clock_skew_minimization_object.h"
#include "network.h"
#include "message_types.h"
#include "core.h"
#include "thread.h"
#include "packetize.h"

ThreadManager::ThreadManager(CoreManager *core_manager)
   : m_thread_spawn_sem(0)
   , m_thread_spawn_lock()
   , m_core_manager(core_manager)
   , m_thread_spawners_terminated(0)
{
   Config *config = Config::getSingleton();

   m_master = config->getCurrentProcessNum() == 0;

   if (m_master)
   {
      m_thread_state.resize(config->getTotalCores());
      /* In PIN, thread 0 is the main application thread which is always alive */
      if (Sim()->getConfig()->getSimulationMode() == Config::LITE || Sim()->getConfig()->getSimulationMode() == Config::FULL)
         m_thread_state[0].status = Core::RUNNING;

      if (Sim()->getConfig()->getSimulationMode() == Config::FULL)
      {
         // Reserve core-id's 1 to (num_processes) for thread-spawners
         UInt32 first_thread_spawner_id = Sim()->getConfig()->getTotalCores() - Sim()->getConfig()->getProcessCount() - 1;
         UInt32 last_thread_spawner_id = Sim()->getConfig()->getTotalCores() - 2;
         for (UInt32 i = first_thread_spawner_id; i <= last_thread_spawner_id; i++)
         {
            m_thread_state[i].status = Core::RUNNING;
         }
      }

      m_thread_state[config->getMCPCoreNum()].status = Core::RUNNING;

      LOG_ASSERT_ERROR(config->getMCPCoreNum() < (SInt32) m_thread_state.size(),
                       "MCP core num out of range (!?)");
   }
}

ThreadManager::~ThreadManager()
{
   if (m_master)
   {
      m_thread_state[0].status = Core::IDLE;
      m_thread_state[Config::getSingleton()->getMCPCoreNum()].status = Core::IDLE;
      LOG_ASSERT_ERROR(Config::getSingleton()->getMCPCoreNum() < (SInt32)m_thread_state.size(), "MCP core num out of range (!?)");

      if (Sim()->getConfig()->getSimulationMode() == Config::FULL)
      {
         // Reserve core-id's 1 to (num_processes) for thread-spawners
         UInt32 first_thread_spawner_id = Sim()->getConfig()->getTotalCores() - Sim()->getConfig()->getProcessCount() - 1;
         UInt32 last_thread_spawner_id = Sim()->getConfig()->getTotalCores() - 2;
         for (UInt32 i = first_thread_spawner_id; i <= last_thread_spawner_id; i++)
         {
            m_thread_state[i].status = Core::IDLE;
         }
      }

      for (UInt32 i = 0; i < m_thread_state.size(); i++)
         if (m_thread_state[i].status != Core::IDLE)
            fprintf(stderr, "Thread %d still active when ThreadManager destructs\n", i);
   }
}

void ThreadManager::onThreadStart(ThreadSpawnRequest *req)
{
   LOG_PRINT("onThreadStart(%i)", req->core_id);

   m_core_manager->initializeThread(req->core_id);

   if (req->core_id == Sim()->getConfig()->getCurrentThreadSpawnerCoreNum())
      return;

   // Set the CoreState to 'RUNNING'
   m_core_manager->getCurrentCore()->setState(Core::RUNNING);

   // send ack to master
   LOG_PRINT("(5) onThreadStart -- send ack to master; req : { %p, %p, %i, %i }",
             req->func, req->arg, req->requester, req->core_id);

   req->msg_type = MCP_MESSAGE_THREAD_SPAWN_REPLY_FROM_SLAVE;
   Network *net = m_core_manager->getCurrentCore()->getNetwork();
   net->netSend(Config::getSingleton()->getMCPCoreNum(),
                MCP_REQUEST_TYPE,
                req,
                sizeof(*req));

   PerformanceModel *pm = m_core_manager->getCurrentCore()->getPerformanceModel();
   pm->resetElapsedTime();
   pm->queueDynamicInstruction(new SpawnInstruction(req->time));
}

void ThreadManager::onThreadExit()
{
   if (m_core_manager->getCurrentCoreID() == -1)
      return;

   // send message to master process to update thread state
   SInt32 msg[] = { MCP_MESSAGE_THREAD_EXIT, m_core_manager->getCurrentCoreID() };

   LOG_PRINT("onThreadExit -- send message to master ThreadManager; thread %d at time %s",
             m_core_manager->getCurrentCore()->getId(),
             itostr(m_core_manager->getCurrentCore()->getPerformanceModel()->getElapsedTime()).c_str());
   Network *net = m_core_manager->getCurrentCore()->getNetwork();

   // Set the CoreState to 'IDLE'
   m_core_manager->getCurrentCore()->setState(Core::IDLE);

   // terminate thread locally so we are ready for new thread requests
   // on that core
   m_core_manager->terminateThread();

   // update global thread state
   net->netSend(Config::getSingleton()->getMCPCoreNum(),
                MCP_REQUEST_TYPE,
                msg,
                sizeof(SInt32)*2);
}

void ThreadManager::masterOnThreadExit(core_id_t core_id, SubsecondTime time)
{
   LOG_ASSERT_ERROR(m_master, "masterOnThreadExit should only be called on master.");
   LOG_PRINT("masterOnThreadExit : %d", core_id);
   LOG_ASSERT_ERROR((UInt32)core_id < m_thread_state.size(), "Core id out of range: %d", core_id);

   assert(m_thread_state[core_id].status == Core::RUNNING);
   m_thread_state[core_id].status = Core::IDLE;

   HooksManager::ThreadStall args = { core_id: core_id, time: time };
   Sim()->getHooksManager()->callHooks(HookType::HOOK_THREAD_STALL, (void*)&args);
   if (Sim()->getMCP()->getClockSkewMinimizationServer())
      Sim()->getMCP()->getClockSkewMinimizationServer()->signal();

   wakeUpWaiter(core_id, time);

   if (Sim()->getConfig()->getSimulationMode() == Config::FULL)
      slaveTerminateThreadSpawnerAck(core_id);
}

/*
  Thread spawning occurs in the following steps:

  1. A message is sent from requestor to the master thread manager.
  2. The master thread manager finds the destination core and sends a message to its host process.
  3. The host process spawns the new thread by adding the information to a list and posting a sempaphore.
  4. The thread is spawned by the thread spawner coming from the application.
  5. The spawned thread initializes and then sends an ACK to the master thread manager.
  6. The master thread manager replies to the requestor with the id of the dest core.
*/

SInt32 ThreadManager::spawnThread(thread_func_t func, void *arg)
{
   // step 1
   LOG_PRINT("(1) spawnThread with func: %p and arg: %p", func, arg);

   Core *core = m_core_manager->getCurrentCore();
   Network *net = core->getNetwork();

   ThreadSpawnRequest req = { MCP_MESSAGE_THREAD_SPAWN_REQUEST_FROM_REQUESTER,
                              func, arg, core->getId(), INVALID_CORE_ID,
                              core->getPerformanceModel()->getElapsedTime() };

   net->netSend(Config::getSingleton()->getMCPCoreNum(),
                MCP_REQUEST_TYPE,
                &req,
                sizeof(req));

   // Set the CoreState to 'STALLED'
   core->setState(Core::STALLED);

   NetPacket pkt = net->netRecvType(MCP_THREAD_SPAWN_REPLY_FROM_MASTER_TYPE);

   LOG_ASSERT_ERROR(pkt.length == sizeof(SInt32), "Unexpected reply size.");

   // Set the CoreState to 'RUNNING'
   core->setState(Core::RUNNING);

   core_id_t core_id = *((core_id_t*)pkt.data);
   LOG_PRINT("Thread spawned on core: %d", core_id);

   // Delete the data buffer
   delete [] (Byte*) pkt.data;

   return core_id;
}

void ThreadManager::masterSpawnThread(ThreadSpawnRequest *req)
{
   // step 2
   LOG_ASSERT_ERROR(m_master, "masterSpawnThread should only be called on master.");
   LOG_PRINT("(2) masterSpawnThread with req: { %p, %p, %d, %d }", req->func, req->arg, req->requester, req->core_id);

   // find core to use
   // FIXME: Load balancing?
   for (SInt32 i = 0; i < (SInt32)m_thread_state.size(); i++)
   {
      if (m_thread_state[i].status == Core::IDLE)
      {
         req->core_id = i;
         break;
      }
   }

   LOG_ASSERT_ERROR(req->core_id != INVALID_CORE_ID, "No cores available for spawnThread request.");

   if (Sim()->getConfig()->getSimulationMode() == Config::FULL)
   {
      // Mark the requesting thread as stalled
      stallThread(req->requester, SubsecondTime::MaxTime());

      // spawn process on child
      SInt32 dest_proc = Config::getSingleton()->getProcessNumForCore(req->core_id);
      Transport::Node *globalNode = Transport::getSingleton()->getGlobalNode();

      req->msg_type = LCP_MESSAGE_THREAD_SPAWN_REQUEST_FROM_MASTER;

      globalNode->globalSend(dest_proc, req, sizeof(*req));

      LOG_ASSERT_ERROR((UInt32)req->core_id < m_thread_state.size(), "Core id out of range: %d", req->core_id);
   }
   else if (Sim()->getConfig()->getSimulationMode() == Config::LITE)
   {
      LOG_PRINT("New Thread to be spawned with core id(%i)", req->core_id);
      ThreadSpawnRequest *req_cpy = new ThreadSpawnRequest;
      *req_cpy = *req;

      // Insert the request in the thread request queue and
      // the thread request map
      insertThreadSpawnRequest(req_cpy);
      m_thread_spawn_sem.signal();

      Core *core = m_core_manager->getCurrentCore();
      core->getNetwork()->netSend(req->requester,
            MCP_THREAD_SPAWN_REPLY_FROM_MASTER_TYPE,
            &req->core_id,
            sizeof(req->core_id));
   }
   else
   {
      LOG_PRINT_ERROR("Cannot start new threads in this mode");
   }

   LOG_ASSERT_ERROR((UInt32)req->core_id < m_thread_state.size(), "Core id out of range: %d", req->core_id);
   LOG_PRINT("Setting status[%i] -> INITIALIZING", req->core_id);
   m_thread_state[req->core_id].status = Core::INITIALIZING;
   LOG_PRINT("Done with (2)");
}

void ThreadManager::slaveSpawnThread(ThreadSpawnRequest *req)
{
   // step 3
   LOG_PRINT("(3) slaveSpawnThread with req: { fun: %p, arg: %p, req: %d, core: %d }", req->func, req->arg, req->requester, req->core_id);

   // This is deleted after the thread has been spawned
   // and we have sent the acknowledgement back to the requester
   ThreadSpawnRequest *req_cpy = new ThreadSpawnRequest;
   *req_cpy = *req;

   // Insert the request in the thread request queue and
   // the thread request map
   insertThreadSpawnRequest (req_cpy);

   m_thread_spawn_sem.signal();
}

void ThreadManager::getThreadToSpawn(ThreadSpawnRequest *req)
{
   // step 4 - this is called from the thread spawner
   LOG_PRINT("(4a) getThreadToSpawn called by user.");

   // Wait for a request to arrive
   m_thread_spawn_sem.wait();

   // Grab the request and set the argument
   // The lock is released by the spawned thread
   m_thread_spawn_lock.acquire();
   *req = *((ThreadSpawnRequest*) m_thread_spawn_list.front());

   LOG_PRINT("(4b) getThreadToSpawn giving thread %p arg: %p to user.", req->func, req->arg);
}

ThreadSpawnRequest* ThreadManager::getThreadSpawnReq()
{
   if (m_thread_spawn_list.empty())
   {
      return (ThreadSpawnRequest*) NULL;
   }
   else
   {
      return m_thread_spawn_list.front();
   }
}

void ThreadManager::dequeueThreadSpawnReq (ThreadSpawnRequest *req)
{
   ThreadSpawnRequest *thread_req = m_thread_spawn_list.front();

   *req = *thread_req;

   m_thread_spawn_list.pop();
   m_thread_spawn_lock.release();

   delete thread_req;

   LOG_PRINT("Dequeued req: { %p, %p, %d, %d }", req->func, req->arg, req->requester, req->core_id);
}

void ThreadManager::masterSpawnThreadReply(ThreadSpawnRequest *req)
{
   // step 6
   LOG_ASSERT_ERROR(m_master, "masterSpawnThreadReply should only be called on master.");
   LOG_PRINT("(6) masterSpawnThreadReply with req: { fun: %p, arg: %p, req: %d, core: %d }", req->func, req->arg, req->requester, req->core_id);

   // Set the state of the actual thread spawned to running
   LOG_PRINT("Setting status[%i] -> RUNNING", req->core_id);
   m_thread_state[req->core_id].status = Core::RUNNING;

   if (Sim()->getConfig()->getSimulationMode() == Config::FULL)
   {
      // Resume the requesting thread
      resumeThread(req->requester, req->core_id, SubsecondTime::MaxTime());

      Core *core = m_core_manager->getCurrentCore();
      core->getNetwork()->netSend(req->requester,
                                  MCP_THREAD_SPAWN_REPLY_FROM_MASTER_TYPE,
                                  &req->core_id,
                                  sizeof(req->core_id));
   }
}

bool ThreadManager::areAllCoresRunning()
{
   LOG_ASSERT_ERROR(m_master, "areAllCoresRunning() should only be called on master.");
   // Check if all the cores are running
   bool is_all_running = true;
   for (SInt32 i = 0; i < (SInt32) m_thread_state.size(); i++)
   {
      if (m_thread_state[i].status == Core::IDLE)
      {
         is_all_running = false;
         break;
      }
   }

   return is_all_running;
}

void ThreadManager::joinThread(core_id_t core_id)
{
   LOG_PRINT("Joining on core: %d", core_id);

   Core* core = m_core_manager->getCurrentCore();
   SubsecondTime start_time = core->getPerformanceModel()->getElapsedTime();

   // Send the message to the master process; will get reply when thread is finished
   ThreadJoinRequest msg = { MCP_MESSAGE_THREAD_JOIN_REQUEST,
                             core->getId(),
                             core_id };

   Network *net = core->getNetwork();
   net->netSend(Config::getSingleton()->getMCPCoreNum(),
                MCP_REQUEST_TYPE,
                &msg,
                sizeof(msg));

   // Set the CoreState to 'STALLED'
   core->setState(Core::STALLED);

   // Wait for reply
   NetPacket pkt = net->netRecvType(MCP_THREAD_JOIN_REPLY);

   UnstructuredBuffer buff;
   unsigned int dummy;
   SubsecondTime time;
   buff << std::make_pair(pkt.data, pkt.length);
   buff >> dummy;
   buff >> time;
   assert(dummy == MCP_THREAD_JOIN_REPLY);

   if (time > start_time)
       core->getPerformanceModel()->queueDynamicInstruction(new SyncInstruction(time - start_time, SyncInstruction::JOIN));

   // Set the CoreState to 'WAKING_UP'
   core->setState(Core::WAKING_UP);

   LOG_PRINT("Exiting join thread.");
}

void ThreadManager::masterJoinThread(ThreadJoinRequest *req, SubsecondTime time)
{
   LOG_ASSERT_ERROR(m_master, "masterJoinThread should only be called on master.");
   LOG_PRINT("masterJoinThread called on core: %d", req->core_id);
   //FIXME: fill in the proper time

   LOG_ASSERT_ERROR(m_thread_state[req->core_id].waiter == -1,
                    "Multiple threads joining on thread: %d", req->core_id);
   m_thread_state[req->core_id].waiter = req->sender;

   // Stall the 'pthread_join' caller
   stallThread(req->sender, time);

   // Core not running, so the thread must have joined
   LOG_ASSERT_ERROR((UInt32)req->core_id < m_thread_state.size(), "Core id out of range: %d", req->core_id);
   if (m_thread_state[req->core_id].status == Core::IDLE)
   {
      LOG_PRINT("Not running, sending reply.");
      wakeUpWaiter(req->core_id, time);
   }
}

void ThreadManager::wakeUpWaiter(core_id_t core_id, SubsecondTime time)
{
   if (m_thread_state[core_id].waiter != INVALID_CORE_ID)
   {
      LOG_PRINT("Waking up core: %d at time: %s", m_thread_state[core_id].waiter, itostr(time).c_str());

      Core *core = m_core_manager->getCurrentCore();
      core_id_t dest = m_thread_state[core_id].waiter;

      // Resume the 'pthread_join' caller
      resumeThread(dest, core_id, time);

      Reply r;
      r.dummy = MCP_THREAD_JOIN_REPLY;
      r.time = time;
      core->getNetwork()->netSend(dest, MCP_THREAD_JOIN_REPLY, (char*)&r, sizeof(r));

      m_thread_state[core_id].waiter = INVALID_CORE_ID;
   }
   LOG_PRINT("Exiting wakeUpWaiter");
}

void ThreadManager::insertThreadSpawnRequest(ThreadSpawnRequest *req)
{
   // Insert the request in the thread request queue
   m_thread_spawn_lock.acquire();
   m_thread_spawn_list.push(req);
   m_thread_spawn_lock.release();
}

void ThreadManager::terminateThreadSpawners()
{
   LOG_PRINT ("In terminateThreadSpawner");

   Transport::Node *node = Transport::getSingleton()->getGlobalNode();

   assert(node);

   for (UInt32 pid = 0; pid < Config::getSingleton()->getProcessCount(); pid++)
   {
      int send_req = LCP_MESSAGE_QUIT_THREAD_SPAWNER;

      LOG_PRINT ("Sending thread spawner quit message to proc %d", pid);
      node->globalSend (pid, &send_req, sizeof (send_req));
      LOG_PRINT ("Sent thread spawner quit message to proc %d", pid);
   }

   // wait for all thread spawners to terminate
   while (true)
   {
      {
         ScopedLock sl(m_thread_spawners_terminated_lock);
         if (m_thread_spawners_terminated < Config::getSingleton()->getProcessCount())
            break;
      }
      sched_yield();
   }
}

void ThreadManager::slaveTerminateThreadSpawner()
{
   LOG_PRINT ("slaveTerminateThreadSpawner on proc %d", Config::getSingleton()->getCurrentProcessNum());

   ThreadSpawnRequest *req = new ThreadSpawnRequest;

   req->msg_type = LCP_MESSAGE_QUIT_THREAD_SPAWNER;
   req->func = NULL;
   req->arg = NULL;
   req->requester = INVALID_CORE_ID;
   req->core_id = INVALID_CORE_ID;

   insertThreadSpawnRequest(req);
   m_thread_spawn_sem.signal();
}

void ThreadManager::slaveTerminateThreadSpawnerAck(core_id_t core_id)
{
   Config *config = Config::getSingleton();
   for (UInt32 i = 0; i < config->getProcessCount(); i++)
   {
      if (core_id == config->getThreadSpawnerCoreNum(i))
      {
         Transport::Node *node = m_core_manager->getCurrentCore()->getNetwork()->getTransport();

         int req_type = LCP_MESSAGE_QUIT_THREAD_SPAWNER_ACK;

         node->globalSend(0, &req_type, sizeof(req_type));
      }
   }
}

void ThreadManager::updateTerminateThreadSpawner()
{
   assert(Config::getSingleton()->getCurrentProcessNum() == 0);
   ScopedLock sl(m_thread_spawners_terminated_lock);
   ++m_thread_spawners_terminated;
}

void ThreadManager::stallThread(core_id_t core_id, SubsecondTime time)
{
   LOG_PRINT("Core(%i) -> STALLED", core_id);
   LOG_ASSERT_ERROR(m_master, "stallThread() must only be called on master");
   m_thread_state[core_id].status = Core::STALLED;

   HooksManager::ThreadStall args = { core_id: core_id, time: time };
   Sim()->getHooksManager()->callHooks(HookType::HOOK_THREAD_STALL, (void*)&args);
   if (Sim()->getMCP()->getClockSkewMinimizationServer())
      Sim()->getMCP()->getClockSkewMinimizationServer()->signal();
}

void ThreadManager::resumeThread(core_id_t core_id, core_id_t core_id_by, SubsecondTime time)
{
   LOG_PRINT("Core(%i) -> RUNNING", core_id);
   LOG_ASSERT_ERROR(m_master, "resumeThread() must only be called on master");
   m_thread_state[core_id].status = Core::RUNNING;

   HooksManager::ThreadResume args = { core_id: core_id, core_by: core_id_by, time: time };
   Sim()->getHooksManager()->callHooks(HookType::HOOK_THREAD_RESUME, (void*)&args);
}

bool ThreadManager::claimThread(core_id_t core_id)
{
   // TODO: we should probably take a lock, or go through the MCP...
   if (m_thread_state[core_id].status != Core::IDLE)
      return false;

   m_thread_state[core_id].status = Core::RUNNING;
   return true;
}

bool ThreadManager::isThreadRunning(core_id_t core_id)
{
   LOG_ASSERT_ERROR(m_master, "isThreadRunning() must only be called on master");
   return (m_thread_state[core_id].status == Core::RUNNING);
}

bool ThreadManager::isThreadInitializing(core_id_t core_id)
{
   LOG_ASSERT_ERROR(m_master, "isThreadInitializing() must only be called on master");
   return (m_thread_state[core_id].status == Core::INITIALIZING);
}
