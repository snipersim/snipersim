#include "dynamic_instruction.h"
#include "instruction.h"
#include "allocator.h"
#include "core.h"
#include "branch_predictor.h"
#include "performance_model.h"

Allocator* DynamicInstruction::createAllocator()
{
   return new TypedAllocator<DynamicInstruction, 1024>();
}

DynamicInstruction::~DynamicInstruction()
{
   if (instruction->isPseudo())
      delete instruction;
}

SubsecondTime DynamicInstruction::getCost(Core *core)
{
   if (isBranch())
      return getBranchCost(core);
   else
      return instruction->getCost(core);
}

SubsecondTime DynamicInstruction::getBranchCost(Core *core, bool *p_is_mispredict)
{
   PerformanceModel *perf = core->getPerformanceModel();
   BranchPredictor *bp = perf->getBranchPredictor();
   const ComponentPeriod *period = core->getDvfsDomain();

   bool is_mispredict = core->accessBranchPredictor(eip, branch_info.taken, branch_info.target);
   UInt64 cost = is_mispredict ? bp->getMispredictPenalty() : 1;

   if (p_is_mispredict)
      *p_is_mispredict = is_mispredict;

   return static_cast<SubsecondTime>(*period) * cost;
}

void DynamicInstruction::accessMemory(Core *core)
{
   for(UInt8 idx = 0; idx < num_memory; ++idx)
   {
      if (memory_info[idx].executed && memory_info[idx].hit_where == HitWhere::UNKNOWN)
      {
         MemoryResult res = core->accessMemory(
            /*instruction.isAtomic()
               ? (info->type == DynamicInstructionInfo::MEMORY_READ ? Core::LOCK : Core::UNLOCK)
               :*/ Core::NONE, // Just as in pin/lite/memory_modeling.cc, make the second part of an atomic update implicit
            memory_info[idx].dir == Operand::READ ? (instruction->isAtomic() ? Core::READ_EX : Core::READ) : Core::WRITE,
            memory_info[idx].addr,
            NULL,
            memory_info[idx].size,
            Core::MEM_MODELED_RETURN,
            instruction->getAddress()
         );
         memory_info[idx].latency = res.latency;
         memory_info[idx].hit_where = res.hit_where;
      }
      else
      {
         memory_info[idx].latency = 1 * core->getDvfsDomain()->getPeriod(); // 1 cycle latency
         memory_info[idx].hit_where = HitWhere::PREDICATE_FALSE;
      }
   }
}
