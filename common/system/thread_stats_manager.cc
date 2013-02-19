#include "thread_stats_manager.h"
#include "simulator.h"
#include "core_manager.h"
#include "performance_model.h"
#include "thread.h"
#include "stats.h"

#include <cstring>

ThreadStatsManager::ThreadStatsManager()
   : m_thread_stat_names(NUM_THREAD_STAT_TYPES, NULL)
   , m_thread_stat_callbacks(NUM_THREAD_STAT_TYPES, NULL)
{
   // Order our hooks to occur before possible reschedulings (which are done with ORDER_ACTION), so the scheduler can use up-to-date information
   Sim()->getHooksManager()->registerHook(HookType::HOOK_PRE_STAT_WRITE, hook_pre_stat_write, (UInt64)this, HooksManager::ORDER_NOTIFY_PRE);
   Sim()->getHooksManager()->registerHook(HookType::HOOK_THREAD_START, hook_thread_start, (UInt64)this, HooksManager::ORDER_NOTIFY_PRE);
   Sim()->getHooksManager()->registerHook(HookType::HOOK_THREAD_STALL, hook_thread_stall, (UInt64)this, HooksManager::ORDER_NOTIFY_PRE);
   Sim()->getHooksManager()->registerHook(HookType::HOOK_THREAD_RESUME, hook_thread_resume, (UInt64)this, HooksManager::ORDER_NOTIFY_PRE);
   Sim()->getHooksManager()->registerHook(HookType::HOOK_THREAD_EXIT, hook_thread_exit, (UInt64)this, HooksManager::ORDER_NOTIFY_PRE);

   registerThreadStatMetric(INSTRUCTIONS, "instruction_count", metricCallback);
   registerThreadStatMetric(ELAPSED_TIME, "core_elapsed_time", metricCallback);
   registerThreadStatMetric(ELAPSED_NONIDLE_TIME, "nonidle_elapsed_time", metricCallback);
}

ThreadStatsManager::~ThreadStatsManager()
{
   for(std::unordered_map<thread_id_t, ThreadStats*>::iterator it = m_threads_stats.begin(); it != m_threads_stats.end(); ++it)
      delete it->second;
}

void ThreadStatsManager::registerThreadStatMetric(ThreadStatType type, const char* name, ThreadStatCallback func)
{
   m_thread_stat_names[type] = name;
   m_thread_stat_callbacks[type] = func;
}

void ThreadStatsManager::update(thread_id_t thread_id, SubsecondTime time)
{
   if (time == SubsecondTime::MaxTime())
      time = Sim()->getClockSkewMinimizationServer()->getGlobalTime();

   if (thread_id == INVALID_THREAD_ID)
   {
      for(std::unordered_map<thread_id_t, ThreadStats*>::iterator it = m_threads_stats.begin(); it != m_threads_stats.end(); ++it)
         it->second->update(time);
   }
   else
   {
      m_threads_stats[thread_id]->update(time);
   }
}

UInt64 ThreadStatsManager::metricCallback(ThreadStatType type, thread_id_t thread_id, Core *core)
{
   switch(type)
   {
      case INSTRUCTIONS:
         return core->getPerformanceModel()->getInstructionCount();
      case ELAPSED_TIME:
         return core->getPerformanceModel()->getElapsedTime().getFS();
      case ELAPSED_NONIDLE_TIME:
         return core->getPerformanceModel()->getNonIdleElapsedTime().getFS();
      default:
         LOG_PRINT_ERROR("Invalid ThreadStatType(%d) for this callback", type);
   }
}

void ThreadStatsManager::pre_stat_write()
{
   update();
}

void ThreadStatsManager::threadStart(thread_id_t thread_id, SubsecondTime time)
{
   m_threads_stats[thread_id] = new ThreadStats(thread_id, time);
   m_threads_stats[thread_id]->update(time); // initialize statistic counters
}

void ThreadStatsManager::threadStall(thread_id_t thread_id, ThreadManager::stall_type_t reason, SubsecondTime time)
{
   m_threads_stats[thread_id]->update(time);
}

void ThreadStatsManager::threadResume(thread_id_t thread_id, thread_id_t thread_by, SubsecondTime time)
{
   m_threads_stats[thread_id]->update(time);
}

void ThreadStatsManager::threadExit(thread_id_t thread_id, SubsecondTime time)
{
   m_threads_stats[thread_id]->update(time);
}

ThreadStatsManager::ThreadStats::ThreadStats(thread_id_t thread_id, SubsecondTime time)
   : m_thread(Sim()->getThreadManager()->getThreadFromID(thread_id))
   , m_core_id(INVALID_CORE_ID)
   , time_by_core(Sim()->getConfig()->getApplicationCores())
   , insn_by_core(Sim()->getConfig()->getApplicationCores())
   , m_time_last(time)
   , m_counts(NUM_THREAD_STAT_TYPES, 0)
   , m_last(NUM_THREAD_STAT_TYPES, 0)
{
   registerStatsMetric("thread", thread_id, "elapsed_time", &m_elapsed_time);
   registerStatsMetric("thread", thread_id, "unscheduled_time", &m_unscheduled_time);
   for (core_id_t core_id = 0; core_id < (core_id_t)Sim()->getConfig()->getApplicationCores(); core_id++)
   {
      registerStatsMetric("thread", thread_id, "time_by_core[" + itostr(core_id) + "]", &time_by_core[core_id]);
      registerStatsMetric("thread", thread_id, "instructions_by_core[" + itostr(core_id) + "]", &insn_by_core[core_id]);
   }
   for(unsigned int type = 0; type < NUM_THREAD_STAT_TYPES; ++type)
   {
      registerStatsMetric("thread", thread_id, Sim()->getThreadStatsManager()->getThreadStatName((ThreadStatType)type), &m_counts[type]);
   }
}

void ThreadStatsManager::ThreadStats::update(SubsecondTime time)
{
   // Increment per-thread statistics based on the progress our core has made since last time
   SubsecondTime time_delta = time - m_time_last;
   if (m_core_id == INVALID_CORE_ID)
   {
      m_elapsed_time += time_delta;
      m_unscheduled_time += time_delta;
   }
   else
   {
      Core *core = Sim()->getCoreManager()->getCoreFromID(m_core_id);
      m_elapsed_time += time_delta;
      time_by_core[core->getId()] += core->getPerformanceModel()->getElapsedTime().getFS() - m_last[ELAPSED_TIME];
      insn_by_core[core->getId()] += core->getPerformanceModel()->getInstructionCount() - m_last[INSTRUCTIONS];
      for(unsigned int type = 0; type < NUM_THREAD_STAT_TYPES; ++type)
      {
         m_counts[type] += Sim()->getThreadStatsManager()->getThreadStatCallback((ThreadStatType)type)((ThreadStatType)type, m_thread->getId(), core) - m_last[type];
      }
   }
   // Take a snapshot of our current core's statistics for later comparison
   Core *core = m_thread->getCore();
   if (core)
   {
      m_core_id = core->getId();
      for(unsigned int type = 0; type < NUM_THREAD_STAT_TYPES; ++type)
      {
         m_last[type] = Sim()->getThreadStatsManager()->getThreadStatCallback((ThreadStatType)type)((ThreadStatType)type, m_thread->getId(), core);
      }
   }
   else
      m_core_id = INVALID_CORE_ID;

   m_time_last = time;
}
