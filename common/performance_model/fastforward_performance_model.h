#ifndef FASTFORWARD_PERFORMANCE_MODEL_H
#define FASTFORWARD_PERFORMANCE_MODEL_H

#include "performance_model.h"

class FastforwardPerformanceModel
{
   private:
      Core *m_core;
      PerformanceModel *m_perf;
      const bool m_include_memory_latency;
      const bool m_include_branch_mispredict;
      ComponentLatency m_branch_misprediction_penalty;

      SubsecondTime m_cpi;
      SubsecondTime m_fastforwarded_time;

      SubsecondTime m_cpiBase;
      SubsecondTime m_cpiBranchPredictor;
      std::vector<SubsecondTime> m_cpiDataCache;

   public:
      FastforwardPerformanceModel(Core *core, PerformanceModel *perf);
      ~FastforwardPerformanceModel() {}

      SubsecondTime getCurrentCPI() const { return m_cpi; }
      void setCurrentCPI(SubsecondTime cpi) { m_cpi = cpi; }

      void incrementElapsedTime(SubsecondTime latency);
      void incrementElapsedTime(SubsecondTime latency, SubsecondTime &cpiComponent);
      void notifyElapsedTimeUpdate();
      void countInstructions(IntPtr address, UInt32 count);
      void handleMemoryLatency(SubsecondTime latency, HitWhere::where_t hit_where);
      void handleBranchMispredict();
      void queuePseudoInstruction(PseudoInstruction *i);

      SubsecondTime getFastforwardedTime(void) const { return m_fastforwarded_time; }
};

#endif // FASTFORWARD_PERFORMANCE_MODEL_H
