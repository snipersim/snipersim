#include "fixed_types.h"
#include "instruction_tracer_fpstats.h"
#include "stats.h"
#include "thread_stats_manager.h"
#include "instruction.h"
#include "micro_op.h"

#include <vector>

const char* const fp_iclasses[] = {
   "ADDPD", "ADDSD", "ADDSS", "ADDPS",
   "SUBPD", "SUBSD", "SUBSS", "SUBPS",
   "MULPD", "MULSD", "MULSS", "MULPS",
   "DIVPD", "DIVSD", "DIVSS", "DIVPS",
};

InstructionTracerFPStats::InstructionTracerFPStats(int core_id)
   : m_id(core_id)
{
   std::unordered_map<int, int> iclass2index;
   for (unsigned int i = 0 ; i < (sizeof(fp_iclasses) / sizeof(fp_iclasses[0])); i++)
   {
      int iclass_int = static_cast<int>(str2xed_iclass_enum_t(fp_iclasses[i]));
      m_iclasses[iclass_int] = 0;
      iclass2index[iclass_int] = i;
   }
   for (std::unordered_map<int, uint64_t>::iterator iclass = m_iclasses.begin() ; iclass != m_iclasses.end() ; ++iclass)
   {
      registerStatsMetric("instruction_tracer", core_id, fp_iclasses[iclass2index[iclass->first]], &(iclass->second));
   }
}

void InstructionTracerFPStats::init()
{
   for (unsigned int i = 0 ; i < (sizeof(fp_iclasses) / sizeof(fp_iclasses[0])); i++)
   {
      // registerStat requires static const names
      ThreadStatNamedStat::registerStat(fp_iclasses[i], "instruction_tracer", fp_iclasses[i]);
   }
}

void InstructionTracerFPStats::handleInstruction(Instruction const* instruction)
{
   if (instruction->getMicroOps())
   {
      for (std::vector<const MicroOp *>::const_iterator uop = instruction->getMicroOps()->begin() ; uop != instruction->getMicroOps()->end() ; ++uop)
      {
         int iclass = static_cast<int>((*uop)->getInstructionOpcode());
         if (m_iclasses.count(iclass))
         {
            m_iclasses[iclass]++;
         }
         // The opcode data from the first micro-op is enough
         break;
      }
   }
}
