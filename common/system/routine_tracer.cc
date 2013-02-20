#include "routine_tracer.h"
#include "simulator.h"
#include "thread.h"
#include "core.h"
#include "performance_model.h"
#include "log.h"
#include "stats.h"

RoutineTracer* RoutineTracer::create()
{
   return new RoutineTracer();
}

RoutineTracerThread::RoutineTracerThread(RoutineTracer *master, Thread *thread)
   : m_master(master)
   , m_thread(thread)
{
}

RoutineTracerThread::~RoutineTracerThread()
{
}

void RoutineTracerThread::routineEnter(IntPtr eip)
{
   if (m_stack.size())
      functionChildEnter(m_stack.back(), eip);

   m_stack.push_back(eip);
   functionEnter(eip);
}

void RoutineTracerThread::routineExit(IntPtr eip)
{
   if (m_stack.size() == 0)
      return;

   if (m_stack.back() == eip)
   {
      functionExit(eip);
      m_stack.pop_back();
   }
   else
   {
      bool found = false;
      for(auto it = m_stack.rbegin(); it != m_stack.rend(); ++it)
      {
         if (*it == eip)
         {
            // We found this eip further down the stack: unwind
            while(m_stack.back() != eip)
            {
               functionExit(m_stack.back());
               m_stack.pop_back();
               functionChildExit(m_stack.back(), eip);
            }
            found = true;
            break;
         }
      }
      if (!found)
      {
         // Mismatch, ignore
      }
   }

   if (m_stack.size())
      functionChildExit(m_stack.back(), eip);
}

RTNRoofline::RTNRoofline(RoutineTracer *master, Thread *thread)
   : RoutineTracerThread(master, thread)
{
   m_stat_fp_addsub = Sim()->getStatsManager()->getMetricObject("interval_timer", thread->getId(), "uop_fp_addsub");
   m_stat_fp_muldiv = Sim()->getStatsManager()->getMetricObject("interval_timer", thread->getId(), "uop_fp_muldiv");
   m_stat_l3miss = Sim()->getStatsManager()->getMetricObject("L3", thread->getId(), "load-misses");
}


void RTNRoofline::functionEnter(IntPtr eip)
{
   m_eip = eip;
   m_instruction_count = m_thread->getCore()->getPerformanceModel()->getInstructionCount();
   m_elapsed_time = m_thread->getCore()->getPerformanceModel()->getElapsedTime();
   m_fp_instructions = m_stat_fp_addsub->recordMetric() + m_stat_fp_muldiv->recordMetric();
   m_l3_misses = m_stat_l3miss->recordMetric();
}

void RTNRoofline::functionExit(IntPtr eip)
{
   assert(eip == m_eip);
   m_master->updateRoutine(
      eip, 1,
      m_thread->getCore()->getPerformanceModel()->getInstructionCount() - m_instruction_count,
      m_thread->getCore()->getPerformanceModel()->getElapsedTime() - m_elapsed_time,
      m_stat_fp_addsub->recordMetric() + m_stat_fp_muldiv->recordMetric() - m_fp_instructions,
      m_stat_l3miss->recordMetric() - m_l3_misses
   );
}

void RTNRoofline::functionChildEnter(IntPtr eip, IntPtr eip_parent)
{
   functionExit(eip);
}

void RTNRoofline::functionChildExit(IntPtr eip, IntPtr eip_parent)
{
   functionEnter(eip);
}


RoutineTracer::RoutineTracer()
{
}

RoutineTracer::~RoutineTracer()
{
   writeResults(Sim()->getConfig()->formatOutputFileName("rtntrace.out").c_str());
}

void RoutineTracer::addRoutine(IntPtr eip, const char *name, int column, int line, const char *filename)
{
   ScopedLock sl(m_lock);

   if (m_routines.count(eip) == 0)
   {
      char location[1024];
      snprintf(location, 1023, "%s:%d:%d", filename, line, column);
      location[1023] = '\0';

      m_routines[eip] = new Routine(eip, name, location);
   }
}

void RoutineTracer::updateRoutine(IntPtr eip, UInt64 calls, UInt64 instruction_count, SubsecondTime elapsed_time, UInt64 fp_instructions, UInt64 l3_misses)
{
   ScopedLock sl(m_lock);

   LOG_ASSERT_ERROR(m_routines.count(eip), "Routine %lx not found", eip);

   m_routines[eip]->m_calls += calls;
   m_routines[eip]->m_instruction_count += instruction_count;
   m_routines[eip]->m_elapsed_time += elapsed_time;
   m_routines[eip]->m_fp_instructions += fp_instructions;
   m_routines[eip]->m_l3_misses += l3_misses;
}

RoutineTracerThread* RoutineTracer::getThreadHandler(Thread *thread)
{
   ScopedLock sl(m_lock);

   if (thread->getId() < (thread_id_t)Sim()->getConfig()->getApplicationCores())
   {
      RoutineTracerThread *rtn_thread = new RTNRoofline(this, thread);
      m_threads.push_back(rtn_thread);
      return rtn_thread;
   }
   else
   {
      return NULL;
   }
}

void RoutineTracer::writeResults(const char *filename)
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
