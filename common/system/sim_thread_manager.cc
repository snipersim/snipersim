#include "sim_thread_manager.h"

#include "lock.h"
#include "log.h"
#include "config.h"
#include "simulator.h"
#include "mcp.h"

SimThreadManager::SimThreadManager()
   : m_active_threads(0)
{
}

SimThreadManager::~SimThreadManager()
{
   LOG_ASSERT_WARNING(m_active_threads == 0,
                      "Threads still active when SimThreadManager exits.");
}

void SimThreadManager::spawnSimThreads()
{
   UInt32 num_local_cores = Config::getSingleton()->getNumLocalCores();
   #ifdef ENABLE_PERF_MODEL_OWN_THREAD
   UInt32 num_sim_threads = 2 * num_local_cores;
   #else
   UInt32 num_sim_threads = num_local_cores;
   #endif

   LOG_PRINT("Starting %d threads on proc: %d.", num_sim_threads, Config::getSingleton()->getCurrentProcessNum());

   m_sim_threads = new SimThread [num_local_cores];
   #ifdef ENABLE_PERF_MODEL_OWN_THREAD
   m_core_threads = new CoreThread [num_local_cores];
   #endif

   for (UInt32 i = 0; i < num_local_cores; i++)
   {
      LOG_PRINT("Starting thread %i", i);
      m_sim_threads[i].spawn();
      #ifdef ENABLE_PERF_MODEL_OWN_THREAD
      m_core_threads[i].spawn();
      #endif
   }

// PIN_SpawnInternalThread doesn't schedule its threads until after PIN_StartProgram
//   while (m_active_threads < num_sim_threads)
//      sched_yield();

   LOG_PRINT("Threads started: %d.", m_active_threads);
}

void SimThreadManager::quitSimThreads()
{
   LOG_PRINT("Sending quit messages.");

   Transport::Node *global_node = Transport::getSingleton()->getGlobalNode();
   UInt32 num_local_cores = Config::getSingleton()->getNumLocalCores();

   // This is something of a hard-wired emulation of Network::netSend
   // ... not the greatest thing to do, but whatever.
   NetPacket pkt1(SubsecondTime::Zero(), SIM_THREAD_TERMINATE_THREADS, 0, 0, 0, NULL);
   NetPacket pkt2(SubsecondTime::Zero(), CORE_THREAD_TERMINATE_THREADS, 0, 0, 0, NULL);
   const Config::CoreList &core_list = Config::getSingleton()->getCoreListForProcess(Config::getSingleton()->getCurrentProcessNum());

   for (UInt32 i = 0; i < num_local_cores; i++)
   {
      core_id_t core_id = core_list[i];

      #ifdef ENABLE_PERF_MODEL_OWN_THREAD
      // First kill core thread (needs network thread to be alive to deliver the message)
      pkt2.receiver = core_id;
      global_node->send(core_id, &pkt2, pkt2.bufferSize());
      #endif

      // Now kill network thread
      pkt1.receiver = core_id;
      global_node->send(core_id, &pkt1, pkt1.bufferSize());
   }

   LOG_PRINT("Waiting for local sim threads to exit.");

   while (m_active_threads > 0)
      sched_yield();

   Transport::getSingleton()->barrier();

   LOG_PRINT("All threads have exited.");
}

void SimThreadManager::simThreadStartCallback()
{
   m_active_threads_lock.acquire();
   ++m_active_threads;
   m_active_threads_lock.release();
}

void SimThreadManager::simThreadExitCallback()
{
   m_active_threads_lock.acquire();
   --m_active_threads;
   m_active_threads_lock.release();
}
