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
   m_stat_fp_addsub = Sim()->getStatsManager()->getMetricObject("interval_timer", thread->getId(), "uop_fp_addsub");
   m_stat_fp_muldiv = Sim()->getStatsManager()->getMetricObject("interval_timer", thread->getId(), "uop_fp_muldiv");
   m_stat_l3miss = Sim()->getStatsManager()->getMetricObject("L3", thread->getId(), "load-misses");
}

void RoutineTracerFunctionStats::RtnThread::functionEnter(IntPtr eip)
{
   m_instruction_count = m_thread->getCore()->getPerformanceModel()->getInstructionCount();
   m_elapsed_time = m_thread->getCore()->getPerformanceModel()->getElapsedTime();
   m_fp_instructions = m_stat_fp_addsub->recordMetric() + m_stat_fp_muldiv->recordMetric();
   m_l3_misses = m_stat_l3miss->recordMetric();
}

void RoutineTracerFunctionStats::RtnThread::functionExit(IntPtr eip)
{
   m_master->updateRoutine(
      eip, 1,
      m_thread->getCore()->getPerformanceModel()->getInstructionCount() - m_instruction_count,
      m_thread->getCore()->getPerformanceModel()->getElapsedTime() - m_elapsed_time,
      m_stat_fp_addsub->recordMetric() + m_stat_fp_muldiv->recordMetric() - m_fp_instructions,
      m_stat_l3miss->recordMetric() - m_l3_misses
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

void RoutineTracerFunctionStats::RtnMaster::updateRoutine(IntPtr eip, UInt64 calls, UInt64 instruction_count, SubsecondTime elapsed_time, UInt64 fp_instructions, UInt64 l3_misses)
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
         it->second->m_calls, it->second->m_instruction_count, it->second->m_elapsed_time.getNS(),
         it->second->m_fp_instructions, it->second->m_l3_misses
      );
   }
   fclose(fp);
}
