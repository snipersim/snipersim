#ifndef FASTFORWARD_PERFORMANCE_MODEL_H
#define FASTFORWARD_PERFORMANCE_MODEL_H

#include "performance_model.h"

class FastforwardPerformanceModel
{
   private:
      Core *m_core;
      PerformanceModel *m_perf;

      SubsecondTime m_cpi;
      SubsecondTime m_fastforwarded_time;

   public:
      FastforwardPerformanceModel(Core *core, PerformanceModel *perf);
      ~FastforwardPerformanceModel() {}

      SubsecondTime getCurrentCPI() const { return m_cpi; }
      void setCurrentCPI(SubsecondTime cpi) { m_cpi = cpi; }

      void incrementElapsedTime(SubsecondTime latency);
      void notifyElapsedTimeUpdate();
      void countInstructions(IntPtr address, UInt32 count);
      void queueDynamicInstruction(Instruction *i);
      void queueBasicBlock(BasicBlock *basic_block);

      SubsecondTime getFastforwardedTime(void) const { return m_fastforwarded_time; }
};

#endif // FASTFORWARD_PERFORMANCE_MODEL_H
