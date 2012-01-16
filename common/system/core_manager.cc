#include <sched.h>
#include <linux/unistd.h>
#include <sys/types.h>
#include <sys/syscall.h>
#include <unistd.h>
#include <limits.h>
#include <algorithm>
#include <vector>

#include "core_manager.h"
#include "core.h"
#include "network.h"
#include "cache.h"
#include "config.h"
#include "packetize.h"
#include "message_types.h"

#include "log.h"

CoreManager::CoreManager()
      : m_core_tls(TLS::create())
      , m_core_index_tls(TLS::create())
      , m_thread_type_tls(TLS::create())
      , m_num_registered_sim_threads(0)
      , m_num_registered_core_threads(0)
{
   LOG_PRINT("Starting CoreManager Constructor.");

   UInt32 num_local_cores = Config::getSingleton()->getNumLocalCores();

   UInt32 proc_id = Config::getSingleton()->getCurrentProcessNum();
   const Config::CoreList &local_cores = Config::getSingleton()->getCoreListForProcess(proc_id);

   for (UInt32 i = 0; i < num_local_cores; i++)
   {
      LOG_PRINT("Core[%u] == %d", i, local_cores.at(i));
      m_cores.push_back(new Core(local_cores.at(i)));
      m_initialized_cores.push_back(false);
   }

   LOG_PRINT("Finished CoreManager Constructor.");
}

CoreManager::~CoreManager()
{
   for (std::vector<Core *>::iterator i = m_cores.begin(); i != m_cores.end(); i++)
      delete *i;

   delete m_core_tls;
   delete m_core_index_tls;
   delete m_thread_type_tls;
}

void CoreManager::initializeCommId(SInt32 comm_id)
{
   LOG_PRINT("initializeCommId - current core (id) = %p (%d)", getCurrentCore(), getCurrentCoreID());

   core_id_t core_id = getCurrentCoreID();

   LOG_ASSERT_ERROR(core_id != INVALID_CORE_ID, "Unexpected invalid core id : %d", core_id);

   UnstructuredBuffer send_buff;
   send_buff << (SInt32) LCP_MESSAGE_COMMID_UPDATE << comm_id << core_id;

   LOG_PRINT("Initializing comm_id: %d to core_id: %d", comm_id, core_id);

   // Broadcast this update to other processes

   UInt32 idx = getCurrentCoreIndex();

   LOG_ASSERT_ERROR(idx < Config::getSingleton()->getNumLocalCores(), "CoreManager got and index [%d] out of range (0-%d).",
         idx, Config::getSingleton()->getNumLocalCores());

   Network *network = m_cores.at(idx)->getNetwork();
   Transport::Node *transport = Transport::getSingleton()->getGlobalNode();
   UInt32 num_procs = Config::getSingleton()->getProcessCount();

   for (UInt32 i = 0; i < num_procs; i++)
   {
      transport->globalSend(i,
                            send_buff.getBuffer(),
                            send_buff.size());
   }

   LOG_PRINT("Waiting for replies from LCPs.");

   for (UInt32 i = 0; i < num_procs; i++)
   {
      network->netRecvType(LCP_COMM_ID_UPDATE_REPLY);
      LOG_PRINT("Received reply from proc: %d", i);
   }

   LOG_PRINT("Finished.");
}

void CoreManager::initializeThread()
{
   ScopedLock sl(m_initialized_cores_lock);

   for (core_id_t i = 0; i < (core_id_t)m_initialized_cores.size(); i++)
   {
       if (!m_initialized_cores.at(i))
       {
           doInitializeThread(i);
           return;
       }
   }

   LOG_PRINT_ERROR("initializeThread - No free cores out of %d total.", Config::getSingleton()->getNumLocalCores());
}

void CoreManager::initializeThread(core_id_t core_id)
{
   ScopedLock sl(m_initialized_cores_lock);

   const Config::CoreList &core_list = Config::getSingleton()->getCoreListForProcess(Config::getSingleton()->getCurrentProcessNum());
   LOG_ASSERT_ERROR(core_list.size() == Config::getSingleton()->getNumLocalCores(),
                    "Core list size different from num local cores? %d != %d", core_list.size(), Config::getSingleton()->getNumLocalCores());

   for (UInt32 i = 0; i < core_list.size(); i++)
   {
      core_id_t local_core_id = core_list.at(i);
      if (local_core_id == core_id)
      {
          if (m_initialized_cores.at(i))
              LOG_PRINT_ERROR("initializeThread -- %d/%d already mapped", i, Config::getSingleton()->getNumLocalCores());

          doInitializeThread(i);
          return;
      }
   }

   LOG_PRINT_ERROR("initializeThread - Requested core %d does not live on process %d.", core_id, Config::getSingleton()->getCurrentProcessNum());
}

void CoreManager::doInitializeThread(UInt32 core_index)
{
    m_core_tls->set(m_cores.at(core_index));
    m_core_index_tls->setInt(core_index);
    m_thread_type_tls->setInt(APP_THREAD);
    m_initialized_cores.at(core_index) = true;
    LOG_PRINT("Initialize thread : index %d mapped to core (id): %p (%d)", core_index, m_cores.at(core_index), m_cores.at(core_index)->getId());
    LOG_ASSERT_ERROR(m_core_tls->get() == (void*)(m_cores.at(core_index)),
                     "TLS appears to be broken. %p != %p", m_core_tls->get(), (void*)(m_cores.at(core_index)));
}

void CoreManager::terminateThread()
{
   LOG_ASSERT_WARNING(m_core_tls->get() != NULL, "Thread not initialized while terminating.");

   core_id_t core_index = m_core_index_tls->getInt();
   m_initialized_cores.at(core_index) = false;

   m_core_tls->set(NULL);
   m_core_index_tls->setInt(-1);
}

Core *CoreManager::getCoreFromID(core_id_t id)
{
   Core *core = NULL;
   // Look up the index from the core list
   // FIXME: make this more cached
   const Config::CoreList & cores(Config::getSingleton()->getCoreListForProcess(Config::getSingleton()->getCurrentProcessNum()));
   UInt32 idx = 0;
   for (Config::CLCI i = cores.begin(); i != cores.end(); i++)
   {
      if (*i == id)
      {
         core = m_cores.at(idx);
         break;
      }

      idx++;
   }

   LOG_ASSERT_ERROR(!core || idx < Config::getSingleton()->getNumLocalCores(), "Illegal index in getCoreFromID!");

   return core;
}

UInt32 CoreManager::getCoreIndexFromID(core_id_t core_id)
{
   // Look up the index from the core list
   // FIXME: make this more cached
   const Config::CoreList & cores(Config::getSingleton()->getCoreListForProcess(Config::getSingleton()->getCurrentProcessNum()));
   UInt32 idx = 0;
   for (Config::CLCI i = cores.begin(); i != cores.end(); i++)
   {
      if (*i == core_id)
         return idx;

      idx++;
   }

   LOG_ASSERT_ERROR(false, "Core lookup failed for core id: %d!", core_id);
   return INVALID_CORE_ID;
}

core_id_t CoreManager::registerSimThread(ThreadType type)
{
    if (getCurrentCore() != NULL)
    {
        LOG_PRINT_ERROR("registerSimMemThread - Initialized thread twice");
        return getCurrentCore()->getId();
    }

    ScopedLock sl(m_num_registered_threads_lock);

    UInt32 *num_registered_threads = NULL;
    if (type == SIM_THREAD)
       num_registered_threads = &m_num_registered_sim_threads;
    else if (type == CORE_THREAD)
       num_registered_threads = &m_num_registered_core_threads;
    else
       LOG_ASSERT_ERROR(false, "Unknown thread type %d", type);


    LOG_ASSERT_ERROR(*num_registered_threads < Config::getSingleton()->getNumLocalCores(),
                     "All sim threads already registered. %d > %d",
                     *num_registered_threads+1, Config::getSingleton()->getNumLocalCores());

    Core *core = m_cores.at(*num_registered_threads);

    m_core_tls->set(core);
    m_core_index_tls->setInt(*num_registered_threads);
    m_thread_type_tls->setInt(type);

    ++(*num_registered_threads);

    return core->getId();
}

bool CoreManager::amiSimThread()
{
    return m_thread_type_tls->getInt() == SIM_THREAD;
}

bool CoreManager::amiCoreThread()
{
    return m_thread_type_tls->getInt() == CORE_THREAD;
}

bool CoreManager::amiUserThread()
{
    return m_thread_type_tls->getInt() == APP_THREAD;
}
