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

RoutineTracerFunctionStats::RtnMaster::RtnMaster()
{
   ThreadStatNamedStat::registerStat("fp_addsub", "interval_timer", "uop_fp_addsub");
   ThreadStatNamedStat::registerStat("fp_muldiv", "interval_timer", "uop_fp_muldiv");
   ThreadStatNamedStat::registerStat("l3miss", "L3", "load-misses");
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

   fprintf(fp, "eip\tname\tsource\tcalls");
   for(auto it = types.begin(); it != types.end(); ++it)
      fprintf(fp, "\t%s", Sim()->getThreadStatsManager()->getThreadStatName(*it));
   fprintf(fp, "\n");

   for(auto it = m_routines.begin(); it != m_routines.end(); ++it)
   {
      fprintf(fp, "%lx\t%s\t%s\t%ld", it->second->m_eip, it->second->m_name, it->second->m_location, it->second->m_calls);
      for(auto jt = types.begin(); jt != types.end(); ++jt)
         fprintf(fp, "\t%ld", it->second->m_values[*jt]);
      fprintf(fp, "\n");
   }
   fclose(fp);
}
