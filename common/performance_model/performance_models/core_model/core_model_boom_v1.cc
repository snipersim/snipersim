#include "core_model_boom_v1.h"
#include "riscv_ext.h"
#include "simulator.h"
#include "config.hpp"
#include "interval_contention_boom_v1.h"
#include "rob_contention_boom_v1.h"
#include "dynamic_micro_op_boom_v1.h"
#include "log.h"
#include "config.hpp"
#include "simulator.h"

#include <decoder.h>
#include <riscv_meta.h>

// #include <src/asm/types.h>
// #include <src/asm/meta.h>
// #include <src/asm/codec.h>
// #include <src/asm/switch.h>
// #include <src/asm/strings.h>
// #include <src/asm/host-endian.h>
// #include <src/util/fmt.h>
// #include <src/util/util.h>

// using namespace riscv;

static unsigned int instructionLatencies[rvext::OP_LAST];   // rv8 opcodes, then the vector/matrix extensions 
static unsigned int bypassLatencies[DynamicMicroOpBoomV1::UOP_BYPASS_SIZE];

CoreModelBoomV1::CoreModelBoomV1()
{
      // https://github.com/ucb-bar/riscv-boom/blob/master/src/main/scala/exu/execute.scala
   int dfmaLatency = 4; 
   int imulLatency = 3;
   for (unsigned int i = 0 ; i < rv_op_last ; i++)
   {
      instructionLatencies[i] = 1;
      if (instrlist[i].has_fpu && instrlist[i].has_alu) {
            instructionLatencies[i] = dfmaLatency;
      }
      else if (instrlist[i].has_alu && instrlist[i].has_mul) {
            instructionLatencies[i] = imulLatency;
      }
      else if (instrlist[i].has_alu) {
            instructionLatencies[i] = 1;
      }
      else {
            instructionLatencies[i] = 1;
      }
   }

   /* Vector (RVV) and matrix (IME) instructions: latencies per class from the
      config, with defaults, see config/riscv.cfg. */
   for (unsigned int i = rvext::OP_FIRST; i < rvext::OP_LAST; i++)
      instructionLatencies[i] = extLatency(rvext::class_of(i));
   for (unsigned int c = 0; c < rvext::NUM_CLASSES; c++)
      m_ext_issue_cycles[c] = 1;
   m_ext_issue_cycles[rvext::IME_MMA_INT] = Sim()->getCfg()->getIntDefault("perf_model/core/ime/mma_int_issue_cycles", 4);
   m_ext_issue_cycles[rvext::IME_MMA_FP]  = Sim()->getCfg()->getIntDefault("perf_model/core/ime/mma_fp_issue_cycles", 4);
   m_lll_cutoff = Sim()->getCfg()->getInt("perf_model/core/interval_timer/lll_cutoff");
}

unsigned int CoreModelBoomV1::extLatency(rvext::Class c)
{
   const char *key; int def;
   switch (c)
   {
      case rvext::V_CFG:          key = "perf_model/core/rvv/cfg_latency";        def = 1;  break;
      case rvext::V_LOAD:
      case rvext::V_STORE:        key = "perf_model/core/rvv/memory_latency";     def = 1;  break;
      case rvext::V_INT_ALU:      key = "perf_model/core/rvv/int_alu_latency";    def = 2;  break;
      case rvext::V_INT_MUL:      key = "perf_model/core/rvv/int_mul_latency";    def = 4;  break;
      case rvext::V_INT_DIV:      key = "perf_model/core/rvv/int_div_latency";    def = 20; break;
      case rvext::V_FP_ADDSUB:    key = "perf_model/core/rvv/fp_addsub_latency";  def = 4;  break;
      case rvext::V_FP_MULADD:    key = "perf_model/core/rvv/fp_muladd_latency";  def = 4;  break;
      case rvext::V_FP_DIV:       key = "perf_model/core/rvv/fp_div_latency";     def = 20; break;
      case rvext::IME_CFG:        key = "perf_model/core/ime/cfg_latency";        def = 1;  break;
      case rvext::IME_TILE_LOAD:
      case rvext::IME_TILE_STORE: key = "perf_model/core/ime/tile_latency";       def = 1;  break;
      case rvext::IME_MMA_INT:    key = "perf_model/core/ime/mma_int_latency";    def = 8;  break;
      case rvext::IME_MMA_FP:     key = "perf_model/core/ime/mma_fp_latency";     def = 12; break;
      default:                    return 1;
   }
   return Sim()->getCfg()->getIntDefault(key, def);
}

unsigned int CoreModelBoomV1::getInstructionLatency(const MicroOp *uop) const
{
   unsigned int instruction_type = (unsigned int) uop->getInstructionOpcode();
   /* Opcode 0 is what the decoder leaves for an encoding it does not know --
      vector and matrix extensions ahead of rv8's tables.  Cost it as a
      generic instruction; the decoder has warned about the encoding once. */
   if (instruction_type == 0)
      return 1;
   LOG_ASSERT_ERROR(instruction_type > 0 && instruction_type < rvext::OP_LAST, "Invalid instruction type %d", instruction_type);
   return instructionLatencies[instruction_type];
}

unsigned int CoreModelBoomV1::getAluLatency(const MicroOp *uop) const
{
   /* How long the unit stays busy.  The matrix unit accepts a new MMA every
      issue-cycles (pipelined); vector dividers are busy for the whole latency. */
   const unsigned int op = uop->getInstructionOpcode();
   if (rvext::is_ext(op))
   {
      const rvext::Class c = rvext::class_of(op);
      if (c == rvext::IME_MMA_INT || c == rvext::IME_MMA_FP)
         return m_ext_issue_cycles[c];
      return getInstructionLatency(uop);
   }
   switch(uop->getInstructionOpcode()) {
      case rv_op_div:
      case rv_op_divu:
      case rv_op_divw:
      case rv_op_divuw:
      case rv_op_divd:
      case rv_op_divud:
      case rv_op_fdiv_s:
      case rv_op_fdiv_d:
      case rv_op_fdiv_q:
         return 32;     // TODO: Latency of div operations need to be more accurately determined
      default:
         return getInstructionLatency(uop);
         //LOG_PRINT_ERROR("Don't know the ALU latency for this MicroOp.");
   }
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
