#ifndef __SCHEDULER_ROUND_ROBIN_H
#define __SCHEDULER_ROUND_ROBIN_H

#include "scheduler_dynamic.h"
#include "simulator.h"

class SchedulerPinned : public SchedulerDynamic
{
   public:
      SchedulerPinned(ThreadManager *thread_manager);

      virtual core_id_t threadCreate(thread_id_t);
      virtual void threadYield(thread_id_t thread_id);
      virtual bool threadSetAffinity(thread_id_t calling_thread_id, thread_id_t thread_id, size_t cpusetsize, const cpu_set_t *mask);
      virtual bool threadGetAffinity(thread_id_t thread_id, size_t cpusetsize, cpu_set_t *mask);

      virtual void periodic(SubsecondTime time);
      virtual void threadStart(thread_id_t thread_id, SubsecondTime time);
      virtual void threadStall(thread_id_t thread_id, ThreadManager::stall_type_t reason, SubsecondTime time);
      virtual void threadResume(thread_id_t thread_id, thread_id_t thread_by, SubsecondTime time);
      virtual void threadExit(thread_id_t thread_id, SubsecondTime time);

   private:
      class ThreadInfo
      {
         public:
            ThreadInfo()
               : m_core_affinity(Sim()->getConfig()->getApplicationCores(), false)
               , m_core_running(INVALID_CORE_ID)
               , m_last_scheduled(SubsecondTime::Zero())
            {}
            /* affinity */
            void clearAffinity()
            {
               for(auto it = m_core_affinity.begin(); it != m_core_affinity.end(); ++it)
                  *it = false;
            }
            void setAffinitySingle(core_id_t core_id)
            {
               clearAffinity();
               addAffinity(core_id);
            }
            void addAffinity(core_id_t core_id) { m_core_affinity[core_id] = true; }
            bool hasAffinity(core_id_t core_id) const { return m_core_affinity[core_id]; }
            String getAffinityString() const;
            /* running on core */
            void setCoreRunning(core_id_t core_id) { m_core_running = core_id; }
            core_id_t getCoreRunning() const { return m_core_running; }
            bool isRunning() const { return m_core_running != INVALID_CORE_ID; }
            /* last scheduled */
            void setLastScheduled(SubsecondTime time) { m_last_scheduled = time; }
            SubsecondTime getLastScheduled() const { return m_last_scheduled; }
         private:
            std::vector<bool> m_core_affinity;
            core_id_t m_core_running;
            SubsecondTime m_last_scheduled;
      };

      // Configuration
      const SubsecondTime m_quantum;
      const int m_interleaving;
      std::vector<bool> m_core_mask;
      // Global state
      core_id_t m_next_core;
      SubsecondTime m_last_periodic;
      // Keyed by thread_id
      std::vector<ThreadInfo> m_thread_info;
      // Keyed by core_id
      std::vector<thread_id_t> m_core_thread_running;
      std::vector<SubsecondTime> m_quantum_left;

      core_id_t getNextCore(core_id_t core_id);
      core_id_t findFreeCoreForThread(thread_id_t thread_id);
      void reschedule(SubsecondTime time, core_id_t core_id, bool is_periodic);
      void printState();
};

#endif // __SCHEDULER_ROUND_ROBIN_H
