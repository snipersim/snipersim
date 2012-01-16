#ifndef CORE_MANAGER_H
#define CORE_MANAGER_H

#include "fixed_types.h"
#include "tls.h"
#include "lock.h"
#include "log.h"
#include "core.h"

#include <iostream>
#include <fstream>
#include <map>
#include <vector>

class Core;

class CoreManager
{
   public:
      CoreManager();
      ~CoreManager();

      enum ThreadType {
          INVALID,
          APP_THREAD,   // Application (Pin) thread
          CORE_THREAD,  // Core (Performance model) thread
          SIM_THREAD    // Simulator (Network model) thread
      };

      void initializeCommId(SInt32 comm_id);
      void initializeThread();
      void initializeThread(core_id_t core_id);
      void terminateThread();
      core_id_t registerSimThread(ThreadType type);

      core_id_t getCurrentCoreID(int thread_id = -1) // id of currently active core (or INVALID_CORE_ID)
      {
         Core *core = getCurrentCore(thread_id);
         if (!core)
             return INVALID_CORE_ID;
         else
             return core->getId();
      }
      Core *getCurrentCore(int thread_id = -1)
      {
          return m_core_tls->getPtr<Core>(thread_id);
      }
      UInt32 getCurrentCoreIndex(int thread_id = -1)
      {
          UInt32 idx = m_core_index_tls->getInt(thread_id);
          LOG_ASSERT_ERROR(idx < m_cores.size(), "Invalid core index, idx(%u) >= m_cores.size(%u)", idx, m_cores.size());
          return idx;
      }

      Core *getCoreFromID(core_id_t id);
      Core *getCoreFromIndex(UInt32 index)
      {
         LOG_ASSERT_ERROR(index < Config::getSingleton()->getNumLocalCores(), "getCoreFromIndex -- invalid index %d", index);

         return m_cores.at(index);
      }

      void outputSummary(std::ostream &os);

      UInt32 getCoreIndexFromID(core_id_t core_id);

      bool amiUserThread();
      bool amiCoreThread();
      bool amiSimThread();
   private:

      void doInitializeThread(UInt32 core_index);

      UInt32 *tid_map;
      TLS *m_core_tls;
      TLS *m_core_index_tls;
      TLS *m_thread_type_tls;

      std::vector<bool> m_initialized_cores;
      Lock m_initialized_cores_lock;

      UInt32 m_num_registered_sim_threads;
      UInt32 m_num_registered_core_threads;
      Lock m_num_registered_threads_lock;

      std::vector<Core*> m_cores;
};

#endif
