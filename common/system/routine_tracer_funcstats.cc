#include "routine_tracer_funcstats.h"
#include "simulator.h"
#include "core_manager.h"
#include "thread.h"
#include "core.h"
#include "performance_model.h"
#include "log.h"
#include "stats.h"
#include "cache_efficiency_tracker.h"
#include "utils.h"

RoutineTracerFunctionStats::RtnThread::RtnThread(RoutineTracerFunctionStats::RtnMaster *master, Thread *thread)
   : RoutineTracerThread(thread)
   , m_master(master)
   , m_current_eip(0)
{
}

UInt64 RoutineTracerFunctionStats::RtnThread::getThreadStat(ThreadStatsManager::ThreadStatType type)
{
   return Sim()->getThreadStatsManager()->getThreadStatistic(m_thread->getId(), type);
}

void RoutineTracerFunctionStats::RtnThread::functionEnter(IntPtr eip)
{
   Sim()->getThreadStatsManager()->update(m_thread->getId());

   m_current_eip = eip;
   auto& types = Sim()->getThreadStatsManager()->getThreadStatTypes();
   for(auto it = types.begin(); it != types.end(); ++it)
   {
      m_values_start[*it] = getThreadStat(*it);
   }
}

void RoutineTracerFunctionStats::RtnThread::functionExit(IntPtr eip)
{
   Sim()->getThreadStatsManager()->update(m_thread->getId());

   RtnValues values;
   auto& types = Sim()->getThreadStatsManager()->getThreadStatTypes();
   for(auto it = types.begin(); it != types.end(); ++it)
   {
      values[*it] = getThreadStat(*it) - m_values_start[*it];
   }

   m_master->updateRoutine(eip, 1, values);
}

void RoutineTracerFunctionStats::RtnThread::functionChildEnter(IntPtr eip, IntPtr eip_parent)
{
   functionExit(eip);
}

void RoutineTracerFunctionStats::RtnThread::functionChildExit(IntPtr eip, IntPtr eip_parent)
{
   functionEnter(eip);
}

UInt64 RoutineTracerFunctionStats::RtnThread::getCurrentRoutineId()
{
   return m_current_eip;
}

RoutineTracerFunctionStats::RtnMaster::RtnMaster()
{
   ThreadStatNamedStat::registerStat("fp_addsub", "interval_timer", "uop_fp_addsub");
   ThreadStatNamedStat::registerStat("fp_muldiv", "interval_timer", "uop_fp_muldiv");
   ThreadStatNamedStat::registerStat("l3miss", "L3", "load-misses");
   ThreadStatAggregates::registerStats();
   ThreadStatNamedStat::registerStat("cpiBase", "interval_timer", "cpiBase");
   ThreadStatCpiMem::registerStat();
   Sim()->getConfig()->setCacheEfficiencyCallbacks(__ce_get_owner, __ce_notify, (UInt64)this);
}

RoutineTracerFunctionStats::RtnMaster::~RtnMaster()
{
   writeResults(Sim()->getConfig()->formatOutputFileName("rtntrace.out").c_str());
}

UInt64 RoutineTracerFunctionStats::RtnMaster::ce_get_owner(core_id_t core_id)
{
   Thread *thread = Sim()->getCoreManager()->getCoreFromID(core_id)->getThread();
   if (thread && m_threads.count(thread->getId()))
      return m_threads[thread->getId()]->getCurrentRoutineId();
   else
      return 0;
}

void RoutineTracerFunctionStats::RtnMaster::ce_notify(bool on_roi_end, UInt64 owner, CacheBlockInfo::BitsUsedType bits_used, UInt32 bits_total)
{
   ScopedLock sl(m_lock);

   IntPtr eip = owner;
   if (m_routines.count(eip))
   {
      m_routines[eip]->m_bits_used += countBits(bits_used);
      m_routines[eip]->m_bits_total += bits_total;
   }
}

RoutineTracerThread* RoutineTracerFunctionStats::RtnMaster::getThreadHandler(Thread *thread)
{
   RtnThread* thread_handler = new RtnThread(this, thread);
   m_threads[thread->getId()] = thread_handler;
   return thread_handler;
}

void RoutineTracerFunctionStats::RtnMaster::addRoutine(IntPtr eip, const char *name, int column, int line, const char *filename)
{
   ScopedLock sl(m_lock);

   if (m_routines.count(eip) == 0)
   {
      m_routines[eip] = new RoutineTracerFunctionStats::Routine(eip, name, column, line, filename);
   }
}

void RoutineTracerFunctionStats::RtnMaster::updateRoutine(IntPtr eip, UInt64 calls, RtnValues values)
{
   ScopedLock sl(m_lock);

   LOG_ASSERT_ERROR(m_routines.count(eip), "Routine %lx not found", eip);

   m_routines[eip]->m_calls += calls;
   for(auto it = values.begin(); it != values.end(); ++it)
   {
      m_routines[eip]->m_values[it->first] += it->second;
   }
}

void RoutineTracerFunctionStats::RtnMaster::writeResults(const char *filename)
{
   FILE *fp = fopen(filename, "w");

   auto& types = Sim()->getThreadStatsManager()->getThreadStatTypes();

   fprintf(fp, "eip\tname\tsource\tcalls\tbits_used\tbits_total");
   for(auto it = types.begin(); it != types.end(); ++it)
      fprintf(fp, "\t%s", Sim()->getThreadStatsManager()->getThreadStatName(*it));
   fprintf(fp, "\n");

   for(auto it = m_routines.begin(); it != m_routines.end(); ++it)
   {
      fprintf(fp, "%" PRIxPTR "\t%s\t%s\t%" PRId64 "\t%" PRId64 "\t%" PRId64,
         it->second->m_eip, it->second->m_name, it->second->m_location,
         it->second->m_calls, it->second->m_bits_used, it->second->m_bits_total);
      for(auto jt = types.begin(); jt != types.end(); ++jt)
         fprintf(fp, "\t%" PRId64, it->second->m_values[*jt]);
      fprintf(fp, "\n");
   }
   fclose(fp);
}


// Helper class to provide global icount/time statistics

class ThreadStatAggregates
{
   public:
      static void registerStats();
   private:
      static UInt64 callback(ThreadStatsManager::ThreadStatType type, thread_id_t thread_id, Core *core, UInt64 user);
};

void RoutineTracerFunctionStats::ThreadStatAggregates::registerStats()
{
   Sim()->getThreadStatsManager()->registerThreadStatMetric(ThreadStatsManager::DYNAMIC, "global_instructions", callback, (UInt64)GLOBAL_INSTRUCTIONS);
   Sim()->getThreadStatsManager()->registerThreadStatMetric(ThreadStatsManager::DYNAMIC, "global_nonidle_elapsed_time", callback, (UInt64)GLOBAL_NONIDLE_ELAPSED_TIME);
}

UInt64 RoutineTracerFunctionStats::ThreadStatAggregates::callback(ThreadStatsManager::ThreadStatType type, thread_id_t thread_id, Core *core, UInt64 user)
{
   UInt64 result = 0;

   switch(user)
   {
      case GLOBAL_INSTRUCTIONS:
         for(core_id_t core_id = 0; core_id < (core_id_t)Sim()->getConfig()->getApplicationCores(); ++core_id)
            result += Sim()->getCoreManager()->getCoreFromID(core_id)->getPerformanceModel()->getInstructionCount();
         return result;

      case GLOBAL_NONIDLE_ELAPSED_TIME:
         for(core_id_t core_id = 0; core_id < (core_id_t)Sim()->getConfig()->getApplicationCores(); ++core_id)
            result += Sim()->getCoreManager()->getCoreFromID(core_id)->getPerformanceModel()->getNonIdleElapsedTime().getFS();
         return result;

      default:
         LOG_PRINT_ERROR("Unexpected user value %d", user);
   }
}


// Helper class to provide simplified cpiMem component

ThreadStatsManager::ThreadStatType RoutineTracerFunctionStats::ThreadStatCpiMem::registerStat()
{
   ThreadStatCpiMem *tsns = new ThreadStatCpiMem();
   return Sim()->getThreadStatsManager()->registerThreadStatMetric(ThreadStatsManager::DYNAMIC, "cpiMem", callback, (UInt64)tsns);
}

RoutineTracerFunctionStats::ThreadStatCpiMem::ThreadStatCpiMem()
   : m_stats(Sim()->getConfig()->getApplicationCores())
{
   for(core_id_t core_id = 0; core_id < (core_id_t)Sim()->getConfig()->getApplicationCores(); ++core_id)
   {
      for (int h = HitWhere::WHERE_FIRST ; h < HitWhere::NUM_HITWHERES ; h++)
      {
         String metricName = "cpiDataCache" + String(HitWhereString((HitWhere::where_t)h));
         StatsMetricBase *m = Sim()->getStatsManager()->getMetricObject("interval_timer", core_id, metricName);
         LOG_ASSERT_ERROR(m != NULL, "Invalid statistic %s.%d.%s", "interval_timer", core_id, metricName.c_str());
         m_stats[core_id].push_back(m);
      }
   }
}

UInt64 RoutineTracerFunctionStats::ThreadStatCpiMem::callback(ThreadStatsManager::ThreadStatType type, thread_id_t thread_id, Core *core, UInt64 user)
{
   ThreadStatCpiMem* tsns = (ThreadStatCpiMem*)user;
   std::vector<StatsMetricBase*>& stats = tsns->m_stats[core->getId()];

   UInt64 result = 0;
   for(std::vector<StatsMetricBase*>::iterator it = stats.begin(); it != stats.end(); ++it)
      result += (*it)->recordMetric();
   return result;
}
