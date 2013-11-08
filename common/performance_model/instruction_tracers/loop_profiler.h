#ifndef __LOOP_PROFILER_H
#define __LOOP_PROFILER_H

#include "instruction_tracer.h"

#include <vector>
#include <unordered_map>

class LoopProfiler : public InstructionTracer
{
   private:
      struct Loop
      {
         Loop(IntPtr _eip, UInt64 _size) : eip(_eip), size(_size), count(0) {}
         static bool cmp(const Loop* a, const Loop* b) { return a->weight() > b->weight(); }
         UInt64 weight() const { return size * count; }

         IntPtr eip;
         UInt64 size;
         UInt64 count;
      };
      const Core *m_core;

      typedef std::vector<Loop*> LoopList;
      typedef std::unordered_map<IntPtr, LoopList> LoopMap;
      LoopMap m_loops;
      UInt64 m_total_instructions;

      IntPtr m_eip_last;

      Loop* findLoop(IntPtr eip, UInt64 size);
      Loop* insertLoop(IntPtr eip, UInt64 size);

   public:
      LoopProfiler(const Core *core);
      virtual ~LoopProfiler();

      virtual void traceInstruction(const DynamicMicroOp *uop, uop_times_t *times);
};

#endif // __LOOP_PROFILER_H
