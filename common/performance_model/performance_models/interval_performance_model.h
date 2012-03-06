#ifndef INTERVAL_PERFORMANCE_MODEL_H
#define INTERVAL_PERFORMANCE_MODEL_H

#include "performance_model.h"
#include "instruction.h"
#include "interval_timer.h"
#include "stats.h"
#include "subsecond_time.h"

#define DEBUG_INSN_LOG 0
#define DEBUG_DYN_INSN_LOG 0
#define DEBUG_CYCLE_COUNT_LOG 0

class IntervalPerformanceModel : public PerformanceModel
{
public:
   IntervalPerformanceModel(Core *core, int misprediction_penalty);
   ~IntervalPerformanceModel();

   void outputSummary(std::ostream &os) const;


protected:
   virtual boost::tuple<uint64_t,uint64_t> simulate(const std::vector<MicroOp>& insts);
   virtual void notifyElapsedTimeUpdate();

private:
   bool handleInstruction(Instruction const* instruction);
   void resetState();

   static MicroOp* m_serialize_uop;
   static MicroOp* m_mfence_uop;

   IntervalTimer interval_timer;

   std::vector<MicroOp> m_current_uops;
   bool m_state_uops_done;
   bool m_state_icache_done;
   UInt64 m_state_num_reads_done;
   UInt64 m_state_num_writes_done;
   UInt64 m_state_num_nonmem_done;
   ComponentPeriod m_state_insn_period;
   const Instruction *m_state_instruction;

   UInt64 m_dyninsn_count;
   UInt64 m_dyninsn_cost;
   UInt64 m_dyninsn_zero_count;

   UInt64 m_mem_dep_mask; // Memory access dependency granularity

#if DEBUG_DYN_INSN_LOG
   FILE *m_dyninsn_log;
#endif
#if DEBUG_INSN_LOG
   FILE *m_insn_log;
#endif
#if DEBUG_CYCLE_COUNT_LOG
   FILE *m_cycle_log;
#endif

   SubsecondTime m_cpiITLBMiss;
   SubsecondTime m_cpiDTLBMiss;
   SubsecondTime m_cpiMemAccess;

};

#endif
