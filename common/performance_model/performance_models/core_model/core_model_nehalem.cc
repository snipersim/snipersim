#include "core_model_nehalem.h"
#include "interval_contention_nehalem.h"
#include "rob_contention_nehalem.h"
#include "dynamic_micro_op_nehalem.h"
#include "log.h"
#include "config.hpp"
#include "simulator.h"

static unsigned int instructionLatencies[XED_ICLASS_LAST];
static unsigned int bypassLatencies[DynamicMicroOpNehalem::UOP_BYPASS_SIZE];

// Intel Nehalem Latencies
// http://www.agner.org/optimize

CoreModelNehalem::CoreModelNehalem()
{
   // Default instruction latency is one cycle
   for (unsigned int i = 0 ; i < XED_ICLASS_LAST ; i++)
   {
      instructionLatencies[i] = 1;
   }

   instructionLatencies[XED_ICLASS_MUL] = 3;
   instructionLatencies[XED_ICLASS_IMUL] = 3;

   instructionLatencies[XED_ICLASS_DIV] = 28; // 32-bit: 17-28, 64-bit: 28-90
   instructionLatencies[XED_ICLASS_IDIV] = 28; // 32-bit: 17-28, 64-bit: 28-90


   instructionLatencies[XED_ICLASS_ADDPS] = 3;
   instructionLatencies[XED_ICLASS_ADDSS] = 3;
   instructionLatencies[XED_ICLASS_ADDSUBPS] = 3;
   instructionLatencies[XED_ICLASS_SUBPS] = 3;
   instructionLatencies[XED_ICLASS_SUBSS] = 3;
   instructionLatencies[XED_ICLASS_VADDPS] = 3;
   instructionLatencies[XED_ICLASS_VADDSS] = 3;
   instructionLatencies[XED_ICLASS_VADDSUBPS] = 3;
   instructionLatencies[XED_ICLASS_VSUBPS] = 3;
   instructionLatencies[XED_ICLASS_VSUBSS] = 3;

   instructionLatencies[XED_ICLASS_ADDPD] = 3;
   instructionLatencies[XED_ICLASS_ADDSD] = 3;
   instructionLatencies[XED_ICLASS_ADDSUBPD] = 3;
   instructionLatencies[XED_ICLASS_SUBPD] = 3;
   instructionLatencies[XED_ICLASS_SUBSD] = 3;
   instructionLatencies[XED_ICLASS_VADDPD] = 3;
   instructionLatencies[XED_ICLASS_VADDSD] = 3;
   instructionLatencies[XED_ICLASS_VADDSUBPD] = 3;
   instructionLatencies[XED_ICLASS_VSUBPD] = 3;
   instructionLatencies[XED_ICLASS_VSUBSD] = 3;

   instructionLatencies[XED_ICLASS_MAXSS] = 3;
   instructionLatencies[XED_ICLASS_MAXSD] = 3;
   instructionLatencies[XED_ICLASS_MAXPS] = 3;
   instructionLatencies[XED_ICLASS_MAXPD] = 3;
   instructionLatencies[XED_ICLASS_MINSS] = 3;
   instructionLatencies[XED_ICLASS_MINSD] = 3;
   instructionLatencies[XED_ICLASS_MINPS] = 3;
   instructionLatencies[XED_ICLASS_MINPD] = 3;

   instructionLatencies[XED_ICLASS_MULSS] = 4;
   instructionLatencies[XED_ICLASS_MULPS] = 4;
   instructionLatencies[XED_ICLASS_VMULSS] = 4;
   instructionLatencies[XED_ICLASS_VMULPS] = 4;

   instructionLatencies[XED_ICLASS_MULSD] = 5;
   instructionLatencies[XED_ICLASS_MULPD] = 5;
   instructionLatencies[XED_ICLASS_VMULSD] = 5;
   instructionLatencies[XED_ICLASS_VMULPD] = 5;

   instructionLatencies[XED_ICLASS_DIVSS] = 11;   // 7-14 (data dependent) according to agner.org
   instructionLatencies[XED_ICLASS_DIVPS] = 11;
   instructionLatencies[XED_ICLASS_VDIVSS] = 11;
   instructionLatencies[XED_ICLASS_VDIVPS] = 11;

   instructionLatencies[XED_ICLASS_DIVSD] = 17;   // 7-22 (data dependent) according to agner.org
   instructionLatencies[XED_ICLASS_DIVPD] = 17;
   instructionLatencies[XED_ICLASS_VDIVSD] = 17;
   instructionLatencies[XED_ICLASS_VDIVPD] = 17;

   instructionLatencies[XED_ICLASS_SQRTSS] = 18;   // 7-18 (data dependent) according to agner.org
   instructionLatencies[XED_ICLASS_SQRTPS] = 18;
   instructionLatencies[XED_ICLASS_VSQRTSS] = 18;
   instructionLatencies[XED_ICLASS_VSQRTPS] = 18;

   instructionLatencies[XED_ICLASS_SQRTSD] = 32;   // 7-32 (data dependent) according to agner.org
   instructionLatencies[XED_ICLASS_SQRTPD] = 32;
   instructionLatencies[XED_ICLASS_VSQRTSD] = 32;
   instructionLatencies[XED_ICLASS_VSQRTPD] = 32;

   instructionLatencies[XED_ICLASS_RSQRTSS] = 3;
   instructionLatencies[XED_ICLASS_RSQRTPS] = 3;
   instructionLatencies[XED_ICLASS_VRSQRTSS] = 3;
   instructionLatencies[XED_ICLASS_VRSQRTPS] = 3;


   instructionLatencies[XED_ICLASS_FLDENV] = 110; // As on X5660 (Westmere)
   instructionLatencies[XED_ICLASS_FNSTENV] = 75; // As on X5660 (Westmere)

   instructionLatencies[XED_ICLASS_CVTPD2PS]  = 4; // (should be 2 uops)
   instructionLatencies[XED_ICLASS_CVTSD2SS]  = 4; // (should be 2 uops)
   instructionLatencies[XED_ICLASS_CVTPS2PD]  = 2; // (should be 2 uops)
   instructionLatencies[XED_ICLASS_CVTSS2SD]  = 1;
   instructionLatencies[XED_ICLASS_CVTDQ2PS]  = 5; // 3 + 2 bypass
   instructionLatencies[XED_ICLASS_CVTPS2DQ]  = 5; // 3 + 2 bypass
   instructionLatencies[XED_ICLASS_CVTTPS2DQ] = 5; // 3 + 2 bypass
   instructionLatencies[XED_ICLASS_CVTDQ2PD]  = 6; // 4 + 2 bypass (should be 2 uops)
   instructionLatencies[XED_ICLASS_CVTPD2DQ]  = 6; // 4 + 2 bypass (should be 2 uops)
   instructionLatencies[XED_ICLASS_CVTTPD2DQ] = 6; // 4 + 2 bypass (should be 2 uops)
   instructionLatencies[XED_ICLASS_CVTPI2PS]  = 5; // 3 + 2 bypass
   instructionLatencies[XED_ICLASS_CVTPS2PI]  = 5; // 3 + 2 bypass
   instructionLatencies[XED_ICLASS_CVTTPS2PI] = 5; // 3 + 2 bypass
   instructionLatencies[XED_ICLASS_CVTPI2PD]  = 6; // (should be 2 uops)
   instructionLatencies[XED_ICLASS_CVTPD2PI]  = 6; // (should be 2 uops)
   instructionLatencies[XED_ICLASS_CVTTPD2PI] = 6;
   instructionLatencies[XED_ICLASS_CVTSI2SS]  = 5; // 3 + 2 bypass
   instructionLatencies[XED_ICLASS_CVTSS2SI]  = 5; // 3 + 2 bypass
   instructionLatencies[XED_ICLASS_CVTTSS2SI] = 5; // 3 + 2 bypass
   instructionLatencies[XED_ICLASS_CVTSI2SD]  = 6; // 4 + 2 bypass (should be 2 uops)
   instructionLatencies[XED_ICLASS_CVTSD2SI]  = 5; // 3 + 2 bypass
   instructionLatencies[XED_ICLASS_CVTTSD2SI] = 5; // 3 + 2 bypass

   instructionLatencies[XED_ICLASS_COMISS] = 3; // 1 + 2 eflags bypass
   instructionLatencies[XED_ICLASS_COMISD] = 3; // 1 + 2 eflags bypass
   instructionLatencies[XED_ICLASS_UCOMISS] = 3; // 1 + 2 eflags bypass
   instructionLatencies[XED_ICLASS_UCOMISD] = 3; // 1 + 2 eflags bypass


   /* bypass latencies */
   /* http://www.agner.org/optimize/microarchitecture.pdf page 86-87 */

   bypassLatencies[DynamicMicroOpNehalem::UOP_BYPASS_NONE] = 0;
   bypassLatencies[DynamicMicroOpNehalem::UOP_BYPASS_LOAD_FP] = 2;
   bypassLatencies[DynamicMicroOpNehalem::UOP_BYPASS_FP_STORE] = 1;


   m_lll_cutoff = Sim()->getCfg()->getInt("perf_model/core/interval_timer/lll_cutoff");
}

unsigned int CoreModelNehalem::getInstructionLatency(const MicroOp *uop) const
{
   xed_iclass_enum_t instruction_type = uop->getInstructionOpcode();
   LOG_ASSERT_ERROR(instruction_type >= 0 && instruction_type < XED_ICLASS_LAST, "Invalid instruction type %d", instruction_type);
   return instructionLatencies[instruction_type];
}

unsigned int CoreModelNehalem::getAluLatency(const MicroOp *uop) const
{
   switch(uop->getInstructionOpcode()) {
      case XED_ICLASS_DIV:
      case XED_ICLASS_IDIV:
         if (uop->getOperandSize() > 32)
            return 28; // Approximate, data-dependent
         else
            return 9;  // Approximate, data-dependent
      default:
         return getInstructionLatency(uop);
         //LOG_PRINT_ERROR("Don't know the ALU latency for this MicroOp.");
   }
}

unsigned int CoreModelNehalem::getBypassLatency(const DynamicMicroOp *uop) const
{
   const DynamicMicroOpNehalem *info = uop->getCoreSpecificInfo<DynamicMicroOpNehalem>();
   DynamicMicroOpNehalem::uop_bypass_t bypass_type = info->getBypassType();
   LOG_ASSERT_ERROR(bypass_type >=0 && bypass_type < DynamicMicroOpNehalem::UOP_BYPASS_SIZE, "Invalid bypass type %d", bypass_type);
   return bypassLatencies[bypass_type];
}

unsigned int CoreModelNehalem::getLongestLatency() const
{
   return 60;
}

IntervalContention* CoreModelNehalem::createIntervalContentionModel(const Core *core) const
{
   return new IntervalContentionNehalem(core, this);
}

RobContention* CoreModelNehalem::createRobContentionModel(const Core *core) const
{
   return new RobContentionNehalem(core, this);
}

DynamicMicroOp* CoreModelNehalem::createDynamicMicroOp(Allocator *alloc, const MicroOp *uop, ComponentPeriod period) const
{
   DynamicMicroOpNehalem *info = DynamicMicroOp::alloc<DynamicMicroOpNehalem>(alloc, uop, this, period);
   info->uop_port = DynamicMicroOpNehalem::getPort(uop);
   info->uop_bypass = DynamicMicroOpNehalem::getBypassType(uop);
   info->uop_alu = DynamicMicroOpNehalem::getAlu(uop);
   return info;
}
