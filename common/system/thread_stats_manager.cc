#include "thread_stats_manager.h"
#include "simulator.h"
#include "core_manager.h"
#include "performance_model.h"
#include "thread.h"
#include "stats.h"

#include <cstring>

ThreadStatsManager::ThreadStatsManager()
   : m_threads_stats(MAX_THREADS)
   , m_thread_stat_types()
   , m_thread_stat_callbacks()
   , m_next_dynamic_type(DYNAMIC)
   , m_bottlegraphs(MAX_THREADS)
   , m_waiting_time_last(SubsecondTime::Zero())
{
   // Order our hooks to occur before possible reschedulings (which are done with ORDER_ACTION), so the scheduler can use up-to-date information
   Sim()->getHooksManager()->registerHook(HookType::HOOK_PRE_STAT_WRITE, hook_pre_stat_write, (UInt64)this, HooksManager::ORDER_NOTIFY_PRE);
   Sim()->getHooksManager()->registerHook(HookType::HOOK_THREAD_CREATE, hook_thread_create, (UInt64)this, HooksManager::ORDER_NOTIFY_PRE);
   Sim()->getHooksManager()->registerHook(HookType::HOOK_THREAD_START, hook_thread_start, (UInt64)this, HooksManager::ORDER_NOTIFY_PRE);
   Sim()->getHooksManager()->registerHook(HookType::HOOK_THREAD_STALL, hook_thread_stall, (UInt64)this, HooksManager::ORDER_NOTIFY_PRE);
   Sim()->getHooksManager()->registerHook(HookType::HOOK_THREAD_RESUME, hook_thread_resume, (UInt64)this, HooksManager::ORDER_NOTIFY_PRE);
   Sim()->getHooksManager()->registerHook(HookType::HOOK_THREAD_EXIT, hook_thread_exit, (UInt64)this, HooksManager::ORDER_NOTIFY_PRE);

   registerThreadStatMetric(INSTRUCTIONS, "instruction_count", metricCallback, 0);
   registerThreadStatMetric(ELAPSED_NONIDLE_TIME, "nonidle_elapsed_time", metricCallback, 0);
   registerThreadStatMetric(WAITING_COST, "waiting_cost", metricCallback, 0);
}

ThreadStatsManager::~ThreadStatsManager()
{
   for(std::vector<ThreadStats*>::iterator it = m_threads_stats.begin(); it != m_threads_stats.end(); ++it)
      if (*it)
         delete *it;
}

ThreadStatsManager::ThreadStatType ThreadStatsManager::registerThreadStatMetric(ThreadStatType type, const char* name, ThreadStatCallback func, UInt64 user)
{
   if (type == DYNAMIC)
   {
      type = m_next_dynamic_type;
      ++m_next_dynamic_type;
   }
   m_thread_stat_types.push_back(type);
   m_thread_stat_callbacks[type] = StatCallback(name, func, user);
   return type;
}

UInt64 ThreadStatsManager::callThreadStatCallback(ThreadStatType type, thread_id_t thread_id, Core *core)
{
   return m_thread_stat_callbacks[type].call(type, thread_id, core);
}

void ThreadStatsManager::update(thread_id_t thread_id, SubsecondTime time)
{
   if (time == SubsecondTime::MaxTime())
      time = Sim()->getClockSkewMinimizationServer()->getGlobalTime();

   calculateWaitingCosts(time);

   if (thread_id == INVALID_THREAD_ID)
   {
      for(thread_id = 0; thread_id < (thread_id_t)Sim()->getThreadManager()->getNumThreads(); ++thread_id)
         if (m_threads_stats[thread_id])
            m_threads_stats[thread_id]->update(time);
   }
   else
   {
      m_threads_stats[thread_id]->update(time);
   }
}

void ThreadStatsManager::calculateWaitingCosts(SubsecondTime time)
{
   // Calculate a waiting cost for all threads.
   // For fully-subscribed systems, the cost is equal to each thread's waiting time.
   // On over-subscribed systems, waiting is Ok as long as there are other threads that can execute,
   // so the cost becomes equal to the number of unused core*cycles, which is spread out over all waiting threads.

   if (time > m_waiting_time_last)
   {
      SubsecondTime time_delta = time - m_waiting_time_last;
      UInt32 n_running = 0, n_stalled = 0, n_total = Sim()->getConfig()->getApplicationCores();
      for(thread_id_t thread_id = 0; thread_id < (thread_id_t)Sim()->getThreadManager()->getNumThreads(); ++thread_id)
      {
         if (Sim()->getThreadManager()->isThreadRunning(thread_id))
            ++n_running;
         else if (Sim()->getThreadManager()->getThreadStallReason(thread_id) == ThreadManager::STALL_UNSCHEDULED)
            ;
         else
            ++n_stalled;
      }

      if (n_stalled && n_running <= n_total)
      {
         SubsecondTime cost = (n_total - n_running) * time_delta / n_stalled;
         for(thread_id_t thread_id = 0; thread_id < (thread_id_t)Sim()->getThreadManager()->getNumThreads(); ++thread_id)
         {
            if (!Sim()->getThreadManager()->isThreadRunning(thread_id)
                && Sim()->getThreadManager()->getThreadStallReason(thread_id) != ThreadManager::STALL_UNSCHEDULED)
            {
               m_threads_stats[thread_id]->m_counts[WAITING_COST] += cost.getFS();
            }
         }
      }
      m_waiting_time_last = time;
   }
}

UInt64 ThreadStatsManager::metricCallback(ThreadStatType type, thread_id_t thread_id, Core *core, UInt64 user)
{
   switch(type)
   {
      case INSTRUCTIONS:
         return core->getPerformanceModel()->getInstructionCount();
      case ELAPSED_NONIDLE_TIME:
         return core->getPerformanceModel()->getNonIdleElapsedTime().getFS();
      case WAITING_COST:
         // Waiting cost is added directly to m_counts[], as it needs to be applied even (especially!) when the thread is not on a core
         return 0;
      default:
         LOG_PRINT_ERROR("Invalid ThreadStatType(%d) for this callback", type);
   }
}

void ThreadStatsManager::pre_stat_write()
{
   SubsecondTime time = Sim()->getClockSkewMinimizationServer()->getGlobalTime();
   calculateWaitingCosts(time);
   m_bottlegraphs.update(time, INVALID_THREAD_ID, false);
   update();
}

void ThreadStatsManager::threadCreate(thread_id_t thread_id)
{
   LOG_ASSERT_ERROR(thread_id < MAX_THREADS, "Too many application threads, increase MAX_THREADS");
   m_threads_stats[thread_id] = new ThreadStats(thread_id);
}

void ThreadStatsManager::threadStart(thread_id_t thread_id, SubsecondTime time)
{
   m_threads_stats[thread_id]->update(time, true);
   m_bottlegraphs.threadStart(thread_id);
   m_bottlegraphs.update(time, thread_id, true);
}

void ThreadStatsManager::threadStall(thread_id_t thread_id, ThreadManager::stall_type_t reason, SubsecondTime time)
{
   calculateWaitingCosts(time);
   m_threads_stats[thread_id]->update(time);
   if (reason != ThreadManager::STALL_UNSCHEDULED)
      m_bottlegraphs.update(time, thread_id, false);
}

void ThreadStatsManager::threadResume(thread_id_t thread_id, thread_id_t thread_by, SubsecondTime time)
{
   calculateWaitingCosts(time);
   m_threads_stats[thread_id]->update(time);
   m_bottlegraphs.update(time, thread_id, true);
}

void ThreadStatsManager::threadExit(thread_id_t thread_id, SubsecondTime time)
{
   calculateWaitingCosts(time);
   m_threads_stats[thread_id]->update(time);
   m_bottlegraphs.update(time, thread_id, false);
}

ThreadStatsManager::ThreadStats::ThreadStats(thread_id_t thread_id)
   : m_thread(Sim()->getThreadManager()->getThreadFromID(thread_id))
   , m_core_id(INVALID_CORE_ID)
   , time_by_core(Sim()->getConfig()->getApplicationCores())
   , insn_by_core(Sim()->getConfig()->getApplicationCores())
   , m_elapsed_time(SubsecondTime::Zero())
   , m_unscheduled_time(SubsecondTime::Zero())
   , m_time_last(SubsecondTime::Zero())
   , m_counts()
   , m_last()
{
   registerStatsMetric("thread", thread_id, "elapsed_time", &m_elapsed_time);
   registerStatsMetric("thread", thread_id, "unscheduled_time", &m_unscheduled_time);
   for (core_id_t core_id = 0; core_id < (core_id_t)Sim()->getConfig()->getApplicationCores(); core_id++)
   {
      registerStatsMetric("thread", thread_id, "time_by_core[" + itostr(core_id) + "]", &time_by_core[core_id]);
      registerStatsMetric("thread", thread_id, "instructions_by_core[" + itostr(core_id) + "]", &insn_by_core[core_id]);
   }
   ThreadStatsManager *tsm = Sim()->getThreadStatsManager();
   for(std::vector<ThreadStatType>::const_iterator it = tsm->getThreadStatTypes().begin(); it != tsm->getThreadStatTypes().end(); ++it)
   {
      m_counts[*it] = 0;
      m_last[*it] = 0;
      registerStatsMetric("thread", thread_id, tsm->getThreadStatName(*it), &m_counts[*it]);
   }
}

void ThreadStatsManager::ThreadStats::update(SubsecondTime time, bool init)
{
   if (Sim()->getThreadManager()->getThreadState(m_thread->getId()) == Core::IDLE
       || Sim()->getThreadManager()->getThreadState(m_thread->getId()) == Core::INITIALIZING)
      return;

   // Increment per-thread statistics based on the progress our core has made since last time
   SubsecondTime time_delta = init || m_time_last > time ? SubsecondTime::Zero() : time - m_time_last;
   if (m_core_id == INVALID_CORE_ID)
   {
      m_elapsed_time += time_delta;
      m_unscheduled_time += time_delta;
   }
   else
   {
      Core *core = Sim()->getCoreManager()->getCoreFromID(m_core_id);
      m_elapsed_time += time_delta;
      time_by_core[core->getId()] += core->getPerformanceModel()->getNonIdleElapsedTime().getFS() - m_last[ELAPSED_NONIDLE_TIME];
      insn_by_core[core->getId()] += core->getPerformanceModel()->getInstructionCount() - m_last[INSTRUCTIONS];
      for(std::unordered_map<ThreadStatType, UInt64>::iterator it = m_counts.begin(); it != m_counts.end(); ++it)
      {
         m_counts[it->first] += Sim()->getThreadStatsManager()->callThreadStatCallback(it->first, m_thread->getId(), core) - m_last[it->first];
      }
   }
   // Take a snapshot of our current core's statistics for later comparison
   Core *core = m_thread->getCore();
   if (core)
   {
      m_core_id = core->getId();
      for(std::unordered_map<ThreadStatType, UInt64>::iterator it = m_counts.begin(); it != m_counts.end(); ++it)
      {
         m_last[it->first] = Sim()->getThreadStatsManager()->callThreadStatCallback(it->first, m_thread->getId(), core);
      }
   }
   else
      m_core_id = INVALID_CORE_ID;

   if (time > m_time_last)
      m_time_last = time;
}

ThreadStatsManager::ThreadStatType ThreadStatNamedStat::registerStat(const char* name, String objectName, String metricName)
{
   if (Sim()->getStatsManager()->getMetricObject(objectName, 0, metricName))
   {
      ThreadStatNamedStat *tsns = new ThreadStatNamedStat(objectName, metricName);
      return Sim()->getThreadStatsManager()->registerThreadStatMetric(ThreadStatsManager::DYNAMIC, name, callback, (UInt64)tsns);
   }
   else
   {
      return ThreadStatsManager::INVALID;
   }
}

ThreadStatNamedStat::ThreadStatNamedStat(String objectName, String metricName)
{
   for(core_id_t core_id = 0; core_id < (core_id_t)Sim()->getConfig()->getApplicationCores(); ++core_id)
   {
      StatsMetricBase *m = Sim()->getStatsManager()->getMetricObject(objectName, core_id, metricName);
      m_stats.push_back(m);
   }
}

UInt64 ThreadStatNamedStat::callback(ThreadStatsManager::ThreadStatType type, thread_id_t thread_id, Core *core, UInt64 user)
{
   StatsMetricBase *m = ((ThreadStatNamedStat*)user)->m_stats[core->getId()];
   if (m)
      return m->recordMetric();
   else
      return 0;
}
