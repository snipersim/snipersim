#ifndef __MICRO_OP_PERFORMANCE_MODEL_H
#define __MICRO_OP_PERFORMANCE_MODEL_H

#include "performance_model.h"
#include "instruction.h"
#include "interval_timer.h"
#include "stats.h"
#include "subsecond_time.h"
#include "dynamic_micro_op.h"

#define DEBUG_INSN_LOG 0
#define DEBUG_DYN_INSN_LOG 0
#define DEBUG_CYCLE_COUNT_LOG 0

class CoreModel;
class Allocator;

class MicroOpPerformanceModel : public PerformanceModel
{
public:
   MicroOpPerformanceModel(Core *core, bool issue_memops);
   ~MicroOpPerformanceModel();

protected:
   const CoreModel *m_core_model;

   virtual boost::tuple<uint64_t,uint64_t> simulate(const std::vector<DynamicMicroOp*>& insts) = 0;
   virtual void notifyElapsedTimeUpdate() = 0;
   void doSquashing(std::vector<DynamicMicroOp*> &current_uops, uint32_t first_squashed = 0);

private:
   void handleInstruction(DynamicInstruction *instruction);

   static MicroOp* m_serialize_uop;
   static MicroOp* m_mfence_uop;
   static MicroOp* m_memaccess_uop;

   Allocator *m_allocator; // Per-thread allocator for DynamicMicroOps
   const bool m_issue_memops;

   std::vector<DynamicMicroOp*> m_current_uops;
   // An std::set would sound like a better choice for these, but since the number of elements
   // is usually small (one or two, except for some rare vector instructions) a linear search
   // is fast enough; while std::vector does *much* fewer memory allocations/deallocations
   std::vector<IntPtr> m_cache_lines_read;
   std::vector<IntPtr> m_cache_lines_written;

   UInt64 m_dyninsn_count;
   UInt64 m_dyninsn_cost;
   UInt64 m_dyninsn_zero_count;

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
   SubsecondTime m_cpiUnknown;
   SubsecondTime m_cpiMemAccess;
};

#endif // __MICRO_OP_PERFORMANCE_MODEL_H
