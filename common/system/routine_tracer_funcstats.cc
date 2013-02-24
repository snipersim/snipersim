#include "routine_tracer_funcstats.h"
#include "simulator.h"
#include "thread.h"
#include "core.h"
#include "performance_model.h"
#include "log.h"
#include "stats.h"

RoutineTracerFunctionStats::RtnThread::RtnThread(RoutineTracerFunctionStats::RtnMaster *master, Thread *thread)
   : RoutineTracerThread(thread)
   , m_master(master)
{
}

UInt64 RoutineTracerFunctionStats::RtnThread::getThreadStat(ThreadStatsManager::ThreadStatType type)
{
   return Sim()->getThreadStatsManager()->getThreadStatistic(m_thread->getId(), type);
}

void RoutineTracerFunctionStats::RtnThread::functionEnter(IntPtr eip)
{
   Sim()->getThreadStatsManager()->update(m_thread->getId());

   m_instruction_count = getThreadStat(ThreadStatsManager::INSTRUCTIONS);
   m_elapsed_time = getThreadStat(ThreadStatsManager::ELAPSED_TIME);
   m_fp_instructions = getThreadStat(m_master->m_ts_fp_addsub) + getThreadStat(m_master->m_ts_fp_muldiv);
   m_l3_misses = getThreadStat(m_master->m_ts_l3miss);
}

void RoutineTracerFunctionStats::RtnThread::functionExit(IntPtr eip)
{
   Sim()->getThreadStatsManager()->update(m_thread->getId());

   m_master->updateRoutine(
      eip, 1,
      getThreadStat(ThreadStatsManager::INSTRUCTIONS) - m_instruction_count,
      getThreadStat(ThreadStatsManager::ELAPSED_TIME) - m_elapsed_time,
      getThreadStat(m_master->m_ts_fp_addsub) + getThreadStat(m_master->m_ts_fp_muldiv) - m_fp_instructions,
      getThreadStat(m_master->m_ts_l3miss) - m_l3_misses
   );
}

void RoutineTracerFunctionStats::RtnThread::functionChildEnter(IntPtr eip, IntPtr eip_parent)
{
   functionExit(eip);
}

void RoutineTracerFunctionStats::RtnThread::functionChildExit(IntPtr eip, IntPtr eip_parent)
{
   functionEnter(eip);
}

RoutineTracerFunctionStats::RtnMaster::RtnMaster()
{
   m_ts_fp_addsub = ThreadStatNamedStat::registerStat("fp_addsub", "interval_timer", "uop_fp_addsub");
   m_ts_fp_muldiv = ThreadStatNamedStat::registerStat("fp_muldiv", "interval_timer", "uop_fp_muldiv");
   m_ts_l3miss = ThreadStatNamedStat::registerStat("l3miss", "L3", "load-misses");
}

RoutineTracerFunctionStats::RtnMaster::~RtnMaster()
{
   writeResults(Sim()->getConfig()->formatOutputFileName("rtntrace.out").c_str());
}

RoutineTracerThread* RoutineTracerFunctionStats::RtnMaster::getThreadHandler(Thread *thread)
{
   return new RtnThread(this, thread);
}

void RoutineTracerFunctionStats::RtnMaster::addRoutine(IntPtr eip, const char *name, int column, int line, const char *filename)
{
   ScopedLock sl(m_lock);

   if (m_routines.count(eip) == 0)
   {
      m_routines[eip] = new RoutineTracerFunctionStats::Routine(eip, name, column, line, filename);
   }
}

void RoutineTracerFunctionStats::RtnMaster::updateRoutine(IntPtr eip, UInt64 calls, UInt64 instruction_count, UInt64 elapsed_time, UInt64 fp_instructions, UInt64 l3_misses)
{
   ScopedLock sl(m_lock);

   LOG_ASSERT_ERROR(m_routines.count(eip), "Routine %lx not found", eip);

   m_routines[eip]->m_calls += calls;
   m_routines[eip]->m_instruction_count += instruction_count;
   m_routines[eip]->m_elapsed_time += elapsed_time;
   m_routines[eip]->m_fp_instructions += fp_instructions;
   m_routines[eip]->m_l3_misses += l3_misses;
}

void RoutineTracerFunctionStats::RtnMaster::writeResults(const char *filename)
{
   FILE *fp = fopen(filename, "w");
   fprintf(fp, "eip\tname\tsource\tcalls\ticount\ttime\tfpinst\tl3miss\n");
   for(auto it = m_routines.begin(); it != m_routines.end(); ++it)
   {
      fprintf(
         fp,
         "%lx\t%s\t%s\t%ld\t%ld\t%ld\t%ld\t%ld\n",
         it->second->m_eip, it->second->m_name, it->second->m_location,
         it->second->m_calls, it->second->m_instruction_count, it->second->m_elapsed_time/1000000,
         it->second->m_fp_instructions, it->second->m_l3_misses
      );
   }
   fclose(fp);
}
