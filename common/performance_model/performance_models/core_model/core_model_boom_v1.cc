#include "core_model_boom_v1.h"
#include "riscv_isa.h"
#include "simulator.h"
#include "config.hpp"
#include "interval_contention_boom_v1.h"
#include "rob_contention_boom_v1.h"
#include "dynamic_micro_op_boom_v1.h"
#include "log.h"
#include "config.hpp"
#include "simulator.h"

#include <decoder.h>



static unsigned int instructionLatencies[rvisa::NUM_OPS];   // by generated opcode id; 0 is "unknown"
static unsigned int bypassLatencies[DynamicMicroOpBoomV1::UOP_BYPASS_SIZE];

CoreModelBoomV1::CoreModelBoomV1()
{
   /* One placeholder latency (and issue interval) per instruction class of
      the generated decoder, from the config with the policy's default:
      perf_model/core/riscv/<class>_latency and _issue_cycles, see
      tools/riscv/isa_policy.yaml and config/riscv.cfg. */
   for (unsigned int c = 0; c < rvisa::NUM_CLASSES; c++)
   {
      const rvisa::ClassInfo &ci = rvisa::class_table[c];
      m_class_latency[c] = Sim()->getCfg()->getIntDefault(String(ci.cfg_key) + "_latency", ci.latency);
      m_class_issue[c] = Sim()->getCfg()->getIntDefault(String(ci.cfg_key) + "_issue_cycles", ci.issue_cycles);
   }
   instructionLatencies[rvisa::OP_UNKNOWN] = 1;
   for (unsigned int op = 1; op < rvisa::NUM_OPS; op++)
      instructionLatencies[op] = m_class_latency[rvisa::class_of(op)];
   m_lll_cutoff = Sim()->getCfg()->getInt("perf_model/core/interval_timer/lll_cutoff");
}

unsigned int CoreModelBoomV1::getInstructionLatency(const MicroOp *uop) const
{
   unsigned int instruction_type = (unsigned int) uop->getInstructionOpcode();
   /* Opcode 0 is what the decoder leaves for an encoding outside the
      generated tables; it is costed as a generic instruction (the decoder
      has warned about the encoding once). */
   LOG_ASSERT_ERROR(instruction_type < rvisa::NUM_OPS, "Invalid instruction type %d", instruction_type);
   return instructionLatencies[instruction_type];
}

unsigned int CoreModelBoomV1::getAluLatency(const MicroOp *uop) const
{
   /* How long the unit stays busy.  Dividers and square roots (FLAG_BUSY)
      are busy for the whole operation; the matrix unit and anything else
      with an issue interval accept a new instruction every that many cycles. */
   const unsigned int op = uop->getInstructionOpcode();
   if (op == rvisa::OP_UNKNOWN)
      return 1;
   const rvisa::Class c = rvisa::class_of(op);
   const uint32_t flags = rvisa::info(c).flags;
   if (flags & rvisa::FLAG_MATRIX)
      return m_class_issue[c];
   if (flags & rvisa::FLAG_BUSY)
      return m_class_latency[c];
   return m_class_issue[c] > 1 ? m_class_issue[c] : getInstructionLatency(uop);
}

unsigned int CoreModelBoomV1::getBypassLatency(const DynamicMicroOp *uop) const
{
   const DynamicMicroOpBoomV1 *info = uop->getCoreSpecificInfo<DynamicMicroOpBoomV1>();
   DynamicMicroOpBoomV1::uop_bypass_t bypass_type = info->getBypassType();
   LOG_ASSERT_ERROR(bypass_type >=0 && bypass_type < DynamicMicroOpBoomV1::UOP_BYPASS_SIZE, "Invalid bypass type %d", bypass_type);
   return bypassLatencies[bypass_type];
}

unsigned int CoreModelBoomV1::getLongestLatency() const
{
   return 60;
}

IntervalContention* CoreModelBoomV1::createIntervalContentionModel(const Core *core) const
{
   return new IntervalContentionBoomV1(core, this);
}

RobContention* CoreModelBoomV1::createRobContentionModel(const Core *core) const
{
   return new RobContentionBoomV1(core, this);
}

DynamicMicroOp* CoreModelBoomV1::createDynamicMicroOp(Allocator *alloc, const MicroOp *uop, ComponentPeriod period) const
{
   DynamicMicroOpBoomV1 *info = DynamicMicroOp::alloc<DynamicMicroOpBoomV1>(alloc, uop, this, period);
   info->uop_port = DynamicMicroOpBoomV1::getPort(uop);
   info->uop_bypass = DynamicMicroOpBoomV1::getBypassType(uop);
   info->uop_alu = DynamicMicroOpBoomV1::getAlu(uop);
   return info;
}
