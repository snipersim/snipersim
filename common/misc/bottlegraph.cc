#include "bottlegraph.h"
#include "thread_manager.h"
#include "stats.h"

// Implement data needed to plot bottle graphs [Du Bois, OOSPLA 2013]

BottleGraphManager::BottleGraphManager(int max_threads)
   : m_running(max_threads, false)
   , m_contrib(max_threads, SubsecondTime::Zero())
   , m_runtime(max_threads, SubsecondTime::Zero())
{
}

void BottleGraphManager::threadStart(thread_id_t thread_id)
{
   m_contrib[thread_id] = SubsecondTime::Zero();
   m_runtime[thread_id] = SubsecondTime::Zero();
   registerStatsMetric("thread", thread_id, "bottle_contrib_time", &m_contrib[thread_id]);
   registerStatsMetric("thread", thread_id, "bottle_runtime_time", &m_runtime[thread_id]);
}

void BottleGraphManager::update(SubsecondTime time, thread_id_t thread_id, bool running)
{
   if (time > m_time_last)
   {
      UInt64 n_running = 0;
      for(thread_id_t _thread_id = 0; _thread_id < (thread_id_t)Sim()->getThreadManager()->getNumThreads(); ++_thread_id)
         if (m_running[_thread_id])
            ++n_running;

      SubsecondTime time_delta = time - m_time_last;
      if (n_running)
         for(thread_id_t _thread_id = 0; _thread_id < (thread_id_t)Sim()->getThreadManager()->getNumThreads(); ++_thread_id)
            if (m_running[_thread_id])
            {
               m_contrib[_thread_id] += time_delta / n_running;
               m_runtime[_thread_id] += time_delta;
            }

      m_time_last = time;
   }

   if (thread_id != INVALID_THREAD_ID)
      m_running[thread_id] = running;
}
