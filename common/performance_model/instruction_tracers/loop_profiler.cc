#include "loop_profiler.h"
#include "dynamic_micro_op.h"
#include "instruction.h"

#include <list>

LoopProfiler::LoopProfiler(const Core *core)
   : m_core(core)
   , m_total_instructions(0)
   , m_eip_last(0)
{
}

LoopProfiler::~LoopProfiler()
{
   std::list<Loop*> loops;
   UInt64 limit = m_total_instructions / 100000;

   for(auto it = m_loops.begin(); it != m_loops.end(); ++it)
   {
      for(auto jt = it->second.begin(); jt != it->second.end(); ++jt)
      {
         if ((*jt)->weight() > limit)
            loops.push_back(*jt);
      }
   }

   loops.sort(Loop::cmp);

   int count = 100;
   for(auto it = loops.begin(); it != loops.end(); ++it)
   {
      printf("%5" PRId64 "x %12" PRIxPTR " .. %12" PRIx64 ": %9" PRId64" (%5.1f%%)\n", (*it)->count, (*it)->eip, (*it)->eip + (*it)->size, (*it)->weight(), 100. * (*it)->weight() / m_total_instructions);
      if (--count == 0)
         break;
   }
}

LoopProfiler::Loop*
LoopProfiler::findLoop(IntPtr eip, UInt64 size)
{
   if (m_loops.count(eip))
      for(auto it = m_loops[eip].begin(); it != m_loops[eip].end(); ++it)
         if ((*it)->size == size)
            return *it;

   return NULL;
}

LoopProfiler::Loop*
LoopProfiler::insertLoop(IntPtr eip, UInt64 size)
{
   Loop *loop = new Loop(eip, size);
   m_loops[eip].push_back(loop);
   return loop;
}

void
LoopProfiler::traceInstruction(const DynamicMicroOp *uop, uop_times_t *times)
{
   // Only consider instructions
   if (!uop->getMicroOp()->isFirst())
      return;
   // Ignore dynamic instructions
   if (!uop->getMicroOp()->getInstruction())
      return;

   IntPtr eip = uop->getMicroOp()->getInstruction()->getAddress();
   ++m_total_instructions;

   // Backwards jump?
   UInt64 size = m_eip_last - eip;
   if (eip < m_eip_last && size < 1000)
   {
      Loop *loop = findLoop(eip, size);

      if (!loop)
         loop = insertLoop(eip, size);

      ++loop->count;
   }

   m_eip_last = eip;
}
