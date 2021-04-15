#include "core_model_cortex_a72.h"
#include "interval_contention_cortex_a72.h"
#include "rob_contention_cortex_a72.h"
#include "dynamic_micro_op_cortex_a72.h"
#include "log.h"
#include "config.hpp"
#include "simulator.h"

#if SNIPER_ARM == 0

#else /* SNIPER_ARM == 1 */

#include <capstone.h>
#include <decoder.h>

static unsigned int instructionLatencies[ARM64_INS_ENDING];
static unsigned int bypassLatencies[DynamicMicroOpCortexA72::UOP_BYPASS_SIZE];

// Cortex A72 latencies
// Cortex-A72 Software Optimization Guide

CoreModelCortexA72::CoreModelCortexA72()
{
   // Default instruction latency is one cycle
   for (unsigned int i = 0 ; i < ARM64_INS_ENDING ; i++)
   {
      instructionLatencies[i] = 1;
   }
   
   // TODO: are LD and ST latencies relevant here?
   
   // Divide and multiply instructions
   instructionLatencies[ARM64_INS_SDIV] = 4;  // 32-bit: 4-12; 64-bit: 4-20
   instructionLatencies[ARM64_INS_UDIV] = 4;  // "
   instructionLatencies[ARM64_INS_MADD] = 3;   // 32-bit: 3; 64-bit: 5 (sequence of instrs can issue every 3 cycles)
   instructionLatencies[ARM64_INS_MSUB] = 3;   // "
   instructionLatencies[ARM64_INS_SMADDL] = 3;
   instructionLatencies[ARM64_INS_SMSUBL] = 3;
   instructionLatencies[ARM64_INS_UMADDL] = 3;
   instructionLatencies[ARM64_INS_UMSUBL] = 3;
   instructionLatencies[ARM64_INS_SMULH] = 9;  // 6 cycles latency + 3 cycles stall 
   instructionLatencies[ARM64_INS_UMULH] = 9;  // "
   
   // Miscellaneous data-processing instructions
   instructionLatencies[ARM64_INS_EXTR] = 3;  // For 2 registers [FIXME for 1 register]
   instructionLatencies[ARM64_INS_BFM] = 2;
   
   // FP data processing instructions
   instructionLatencies[ARM64_INS_FABS] = 3;
   instructionLatencies[ARM64_INS_FADD] = 4;
   instructionLatencies[ARM64_INS_FSUB] = 4;
   instructionLatencies[ARM64_INS_FCCMP] = 3;
   instructionLatencies[ARM64_INS_FCCMPE] = 3;
   instructionLatencies[ARM64_INS_FCMP] = 3;
   instructionLatencies[ARM64_INS_FCMPE] = 3;
   instructionLatencies[ARM64_INS_FDIV] = 12;  // 32-bit: 6-11; 64-bit: 6-18; F64 Q-form: 12-36
   instructionLatencies[ARM64_INS_FMIN] = 3;
   instructionLatencies[ARM64_INS_FMAX] = 3;
   instructionLatencies[ARM64_INS_FMINNM] = 3;
   instructionLatencies[ARM64_INS_FMAXNM] = 3;
   instructionLatencies[ARM64_INS_FMUL] = 4;
   instructionLatencies[ARM64_INS_FNMUL] = 4;
   instructionLatencies[ARM64_INS_FMADD] = 7;
   instructionLatencies[ARM64_INS_FMSUB] = 7;
   instructionLatencies[ARM64_INS_FNMADD] = 7;
   instructionLatencies[ARM64_INS_FNMSUB] = 7;
   instructionLatencies[ARM64_INS_FNEG] = 3;
   instructionLatencies[ARM64_INS_FRINTA] = 3;
   instructionLatencies[ARM64_INS_FRINTI] = 3;
   instructionLatencies[ARM64_INS_FRINTM] = 3;
   instructionLatencies[ARM64_INS_FRINTN] = 3;
   instructionLatencies[ARM64_INS_FRINTP] = 3;
   instructionLatencies[ARM64_INS_FRINTX] = 3;
   instructionLatencies[ARM64_INS_FRINTZ] = 3;
   instructionLatencies[ARM64_INS_FCSEL] = 3;
   instructionLatencies[ARM64_INS_FSQRT] = 17;  // 32-bit: 6-17; 64-bit: 6-32
   
   // FP miscellaneous instructions
   instructionLatencies[ARM64_INS_FCVT] = 3;
   instructionLatencies[ARM64_INS_FCVTXN] = 3;
   instructionLatencies[ARM64_INS_SCVTF] = 8;
   instructionLatencies[ARM64_INS_UCVTF] = 8;
   instructionLatencies[ARM64_INS_FCVTAS] = 8;
   instructionLatencies[ARM64_INS_FCVTAU] = 8;
   instructionLatencies[ARM64_INS_FCVTMS] = 8;
   instructionLatencies[ARM64_INS_FCVTMU] = 8;
   instructionLatencies[ARM64_INS_FCVTNS] = 8;
   instructionLatencies[ARM64_INS_FCVTNU] = 8;
   instructionLatencies[ARM64_INS_FCVTPS] = 8;
   instructionLatencies[ARM64_INS_FCVTPU] = 8;
   instructionLatencies[ARM64_INS_FCVTZS] = 8;
   instructionLatencies[ARM64_INS_FCVTZU] = 8;
   instructionLatencies[ARM64_INS_FMOV] = 5;  // FIXME: 3 if from register or immed; 5 involving vec registers
   
   // ASIMD integer instructions
   instructionLatencies[ARM64_INS_SABD] = 3;
   instructionLatencies[ARM64_INS_UABD] = 3;
   instructionLatencies[ARM64_INS_SABA] = 5;  // 64-bit: 4; 128-bit: 5
   instructionLatencies[ARM64_INS_UABA] = 5;  // "
   instructionLatencies[ARM64_INS_SABAL] = 4; 
   instructionLatencies[ARM64_INS_SABAL2] = 4; 
   instructionLatencies[ARM64_INS_UABAL] = 4; 
   instructionLatencies[ARM64_INS_UABAL2] = 4; 
   instructionLatencies[ARM64_INS_SABDL] = 3;
   instructionLatencies[ARM64_INS_UABDL] = 3;
   instructionLatencies[ARM64_INS_ABS] = 3;
   instructionLatencies[ARM64_INS_ADDP] = 3;
   instructionLatencies[ARM64_INS_NEG] = 3;
   instructionLatencies[ARM64_INS_SADDL] = 3;
   instructionLatencies[ARM64_INS_SADDL2] = 3;
   instructionLatencies[ARM64_INS_SADDLP] = 3;
   instructionLatencies[ARM64_INS_SADDW] = 3;
   instructionLatencies[ARM64_INS_SADDW2] = 3;
   instructionLatencies[ARM64_INS_SHADD] = 3;
   instructionLatencies[ARM64_INS_SHSUB] = 3;
   instructionLatencies[ARM64_INS_SSUBL] = 3;
   instructionLatencies[ARM64_INS_SSUBL2] = 3;
   instructionLatencies[ARM64_INS_SSUBW] = 3;
   instructionLatencies[ARM64_INS_SSUBW2] = 3;
   instructionLatencies[ARM64_INS_UADDL] = 3;
   instructionLatencies[ARM64_INS_UADDL2] = 3;
   instructionLatencies[ARM64_INS_UADDLP] = 3;
   instructionLatencies[ARM64_INS_UADDW] = 3;
   instructionLatencies[ARM64_INS_UADDW2] = 3;
   instructionLatencies[ARM64_INS_UHADD] = 3;
   instructionLatencies[ARM64_INS_UHSUB] = 3;
   instructionLatencies[ARM64_INS_USUBW] = 3;
   instructionLatencies[ARM64_INS_USUBW2] = 3;
   instructionLatencies[ARM64_INS_ADDHN] = 3;
   instructionLatencies[ARM64_INS_ADDHN2] = 3;
   instructionLatencies[ARM64_INS_RADDHN] = 3;
   instructionLatencies[ARM64_INS_RADDHN2] = 3;
   instructionLatencies[ARM64_INS_RSUBHN] = 3;
   instructionLatencies[ARM64_INS_RSUBHN2] = 3;
   instructionLatencies[ARM64_INS_SQABS] = 3;
   instructionLatencies[ARM64_INS_SQADD] = 3;
   instructionLatencies[ARM64_INS_SQNEG] = 3;
   instructionLatencies[ARM64_INS_SQSUB] = 3;
   instructionLatencies[ARM64_INS_SRHADD] = 3;
   instructionLatencies[ARM64_INS_SUBHN] = 3;
   instructionLatencies[ARM64_INS_SUBHN2] = 3;
   instructionLatencies[ARM64_INS_SUQADD] = 3;
   instructionLatencies[ARM64_INS_UQADD] = 3;
   instructionLatencies[ARM64_INS_UQSUB] = 3;
   instructionLatencies[ARM64_INS_URHADD] = 3;
   instructionLatencies[ARM64_INS_USQADD] = 3;
   instructionLatencies[ARM64_INS_ADDV] = 6;
   instructionLatencies[ARM64_INS_SADDLV] = 6;  // 64-bit: 3; 128-bit: 6
   instructionLatencies[ARM64_INS_UADDLV] = 6;  // 64-bit: 3; 128-bit: 6
   instructionLatencies[ARM64_INS_CMEQ] = 3;
   instructionLatencies[ARM64_INS_CMGE] = 3;
   instructionLatencies[ARM64_INS_CMGT] = 3;
   instructionLatencies[ARM64_INS_CMHI] = 3;
   instructionLatencies[ARM64_INS_CMHS] = 3;
   instructionLatencies[ARM64_INS_CMLE] = 3;
   instructionLatencies[ARM64_INS_CMLT] = 3;
   instructionLatencies[ARM64_INS_CMTST] = 3;
   
   instructionLatencies[ARM64_INS_SMAX] = 3;
   instructionLatencies[ARM64_INS_SMAXP] = 3;
   instructionLatencies[ARM64_INS_SMIN] = 3;
   instructionLatencies[ARM64_INS_SMINP] = 3;
   instructionLatencies[ARM64_INS_UMAX] = 3;
   instructionLatencies[ARM64_INS_UMAXP] = 3;
   instructionLatencies[ARM64_INS_UMIN] = 3;
   instructionLatencies[ARM64_INS_UMINP] = 3;
   instructionLatencies[ARM64_INS_SMAXV] = 6;  // 4H/4S: 3; 16B - 8B/8H: 6
   instructionLatencies[ARM64_INS_SMINV] = 6;
   instructionLatencies[ARM64_INS_UMAXV] = 6;
   instructionLatencies[ARM64_INS_UMINV] = 6;
   instructionLatencies[ARM64_INS_MUL] = 3;  // 64-bit: 4; 128-bit: 5
   instructionLatencies[ARM64_INS_PMUL] = 4;
   instructionLatencies[ARM64_INS_SQDMULH] = 5;
   instructionLatencies[ARM64_INS_SQRDMULH] = 5;
   
   instructionLatencies[ARM64_INS_MLA] = 5;   // 64-bit: 4; 128-bit: 5
   instructionLatencies[ARM64_INS_MLS] = 5;   // 64-bit: 4; 128-bit: 5
   instructionLatencies[ARM64_INS_SMLAL] = 4;
   instructionLatencies[ARM64_INS_SMLAL2] = 4;
   instructionLatencies[ARM64_INS_SMLSL] = 4;
   instructionLatencies[ARM64_INS_SMLSL2] = 4;
   instructionLatencies[ARM64_INS_UMLAL] = 4;
   instructionLatencies[ARM64_INS_UMLAL2] = 4;
   instructionLatencies[ARM64_INS_UMLSL] = 4;
   instructionLatencies[ARM64_INS_UMLSL2] = 4;
   instructionLatencies[ARM64_INS_SQDMLAL] = 4;
   instructionLatencies[ARM64_INS_SQDMLAL2] = 4;
   instructionLatencies[ARM64_INS_SQDMLSL] = 4;
   instructionLatencies[ARM64_INS_SQDMLSL2] = 4;
   instructionLatencies[ARM64_INS_SMULL] = 4;
   instructionLatencies[ARM64_INS_SMULL2] = 4;
   instructionLatencies[ARM64_INS_UMULL] = 4;
   instructionLatencies[ARM64_INS_UMULL2] = 4;
   instructionLatencies[ARM64_INS_SQDMULL] = 4;
   instructionLatencies[ARM64_INS_SQDMULL2] = 4;
   instructionLatencies[ARM64_INS_PMULL] = 4;
   instructionLatencies[ARM64_INS_PMULL2] = 4;
   instructionLatencies[ARM64_INS_SADALP] = 4;
   instructionLatencies[ARM64_INS_UADALP] = 4;
//   instructionLatencies[ARM64_INS_SRA] = 4;
   instructionLatencies[ARM64_INS_SRSRA] = 4;
   instructionLatencies[ARM64_INS_USRA] = 4;
   instructionLatencies[ARM64_INS_URSRA] = 4;
   instructionLatencies[ARM64_INS_SHL] = 3;
   instructionLatencies[ARM64_INS_SHLL] = 3;
   instructionLatencies[ARM64_INS_SHLL2] = 3;
   instructionLatencies[ARM64_INS_SHRN] = 3;
   instructionLatencies[ARM64_INS_SHRN2] = 3;
   instructionLatencies[ARM64_INS_SLI] = 4;   // 64-bit: 3; 128-bit: 4
   instructionLatencies[ARM64_INS_SRI] = 4;   // "
   instructionLatencies[ARM64_INS_SSHLL] = 3;
   instructionLatencies[ARM64_INS_SSHLL2] = 3;
   instructionLatencies[ARM64_INS_SSHR] = 3;
//   instructionLatencies[ARM64_INS_SXTL] = 3;
//   instructionLatencies[ARM64_INS_SXTL2] = 3;
   instructionLatencies[ARM64_INS_USHLL] = 3;
   instructionLatencies[ARM64_INS_USHLL2] = 3;
   instructionLatencies[ARM64_INS_USHR] = 3;
//   instructionLatencies[ARM64_INS_UXTL] = 3;
//   instructionLatencies[ARM64_INS_UXTL2] = 3;
   instructionLatencies[ARM64_INS_RSHRN] = 4;
   instructionLatencies[ARM64_INS_RSHRN2] = 4;
   instructionLatencies[ARM64_INS_SRSHR] = 4;
   instructionLatencies[ARM64_INS_SQSHL] = 4;
   instructionLatencies[ARM64_INS_SQSHLU] = 4;
   instructionLatencies[ARM64_INS_SQRSHRN] = 4;
   instructionLatencies[ARM64_INS_SQRSHRN2] = 4;
   instructionLatencies[ARM64_INS_SQRSHRUN] = 4;
   instructionLatencies[ARM64_INS_SQRSHRUN2] = 4;
   instructionLatencies[ARM64_INS_SQSHRN] = 4;
   instructionLatencies[ARM64_INS_SQSHRN2] = 4;
   instructionLatencies[ARM64_INS_SQSHRUN] = 4;
   instructionLatencies[ARM64_INS_SQSHRUN2] = 4;
   instructionLatencies[ARM64_INS_URSHR] = 4;
   instructionLatencies[ARM64_INS_UQSHL] = 4;
   instructionLatencies[ARM64_INS_UQRSHRN] = 4;
   instructionLatencies[ARM64_INS_UQRSHRN2] = 4;
   instructionLatencies[ARM64_INS_UQSHRN] = 4;
   instructionLatencies[ARM64_INS_UQSHRN2] = 4;
   instructionLatencies[ARM64_INS_SSHL] = 4;   // 64-bit: 3; 128-bit: 4
   instructionLatencies[ARM64_INS_USHL] = 4;   // "
   instructionLatencies[ARM64_INS_SRSHL] = 5;  // 64-bit: 4; 128-bit: 5
   instructionLatencies[ARM64_INS_SQRSHL] = 5;
   instructionLatencies[ARM64_INS_SQSHL] = 5;
   instructionLatencies[ARM64_INS_URSHL] = 5;
   instructionLatencies[ARM64_INS_UQRSHL] = 5;
   instructionLatencies[ARM64_INS_UQSHL] = 5;
   
   // ASIMD floating-point instructions
   instructionLatencies[ARM64_INS_FABD] = 4;
   instructionLatencies[ARM64_INS_FADDP] = 7;  // 64-bit: 4; 128-bit: 7
   instructionLatencies[ARM64_INS_FACGE] = 3;
   instructionLatencies[ARM64_INS_FACGT] = 3;
   instructionLatencies[ARM64_INS_FCMEQ] = 3;
   instructionLatencies[ARM64_INS_FCMGE] = 3;
   instructionLatencies[ARM64_INS_FCMGT] = 3;
   instructionLatencies[ARM64_INS_FCMLE] = 3;
   instructionLatencies[ARM64_INS_FCMLT] = 3;
   instructionLatencies[ARM64_INS_FCVTL] = 7;   // F16 to F32: 7; F32 to F64: 3
   instructionLatencies[ARM64_INS_FCVTL2] = 7;  // "   
   instructionLatencies[ARM64_INS_FCVTN] = 7;   // F16 to F32: 7; F32 to F64: 3
   instructionLatencies[ARM64_INS_FCVTN2] = 7;  // "   
   instructionLatencies[ARM64_INS_FCVTXN] = 7;   // F16 to F32: 7; F32 to F64: 3
   instructionLatencies[ARM64_INS_FCVTXN2] = 7;  // "
   instructionLatencies[ARM64_INS_FCVTAS] = 4;
   instructionLatencies[ARM64_INS_FCVTAU] = 4;
   instructionLatencies[ARM64_INS_FCVTMS] = 4;
   instructionLatencies[ARM64_INS_FCVTMU] = 4;
   instructionLatencies[ARM64_INS_FCVTNS] = 4;
   instructionLatencies[ARM64_INS_FCVTNU] = 4;
   instructionLatencies[ARM64_INS_FCVTPS] = 4;
   instructionLatencies[ARM64_INS_FCVTPU] = 4;
   instructionLatencies[ARM64_INS_FCVTZS] = 4;
   instructionLatencies[ARM64_INS_FCVTZU] = 4;
   instructionLatencies[ARM64_INS_SCVTF] = 4;
   instructionLatencies[ARM64_INS_UCVTF] = 4;
   instructionLatencies[ARM64_INS_FMINP] = 3;
   instructionLatencies[ARM64_INS_FMAXP] = 3;
   instructionLatencies[ARM64_INS_FMINNMP] = 3;
   instructionLatencies[ARM64_INS_FMAXNMP] = 3;
   instructionLatencies[ARM64_INS_FMINV] = 6;
   instructionLatencies[ARM64_INS_FMAXV] = 6;
   instructionLatencies[ARM64_INS_FMINNMV] = 6;
   instructionLatencies[ARM64_INS_FMAXNMV] = 6;
   instructionLatencies[ARM64_INS_FMULX] = 4;
   instructionLatencies[ARM64_INS_FMLA] = 7;
   instructionLatencies[ARM64_INS_FMLS] = 7;
   instructionLatencies[ARM64_INS_FNEG] = 3;
   instructionLatencies[ARM64_INS_FRINTA] = 4; // 64-bit: 3; 128-bit: 4
   instructionLatencies[ARM64_INS_FRINTI] = 4;
   instructionLatencies[ARM64_INS_FRINTM] = 4;
   instructionLatencies[ARM64_INS_FRINTN] = 4;
   instructionLatencies[ARM64_INS_FRINTP] = 4;
   instructionLatencies[ARM64_INS_FRINTX] = 4;
   instructionLatencies[ARM64_INS_FRINTZ] = 4;
   
   // ASIMD miscellaneous instructions
   instructionLatencies[ARM64_INS_RBIT] = 3;
   instructionLatencies[ARM64_INS_BIF] = 3;
   instructionLatencies[ARM64_INS_BIT] = 3;
   instructionLatencies[ARM64_INS_BSL] = 3;
   instructionLatencies[ARM64_INS_CLS] = 3;
   instructionLatencies[ARM64_INS_CLZ] = 3;
   instructionLatencies[ARM64_INS_CNT] = 3;
   instructionLatencies[ARM64_INS_DUP] = 8;
   instructionLatencies[ARM64_INS_EXT] = 3;
   instructionLatencies[ARM64_INS_XTN] = 3;
   instructionLatencies[ARM64_INS_SQXTN] = 4;
   instructionLatencies[ARM64_INS_SQXTN2] = 4;
   instructionLatencies[ARM64_INS_SQXTUN] = 4;
   instructionLatencies[ARM64_INS_SQXTUN2] = 4;
   instructionLatencies[ARM64_INS_UQXTN] = 4;
   instructionLatencies[ARM64_INS_UQXTN2] = 4;
   instructionLatencies[ARM64_INS_INS] = 3;
   instructionLatencies[ARM64_INS_MOVI] = 3;
   instructionLatencies[ARM64_INS_FRECPE] = 4;  // 64-bit: 3; 128-bit: 4
   instructionLatencies[ARM64_INS_FRECPX] = 4;
   instructionLatencies[ARM64_INS_FRSQRTE] = 4;
   instructionLatencies[ARM64_INS_URECPE] = 4;
   instructionLatencies[ARM64_INS_URSQRTE] = 4;
   instructionLatencies[ARM64_INS_FRECPS] = 7;
   instructionLatencies[ARM64_INS_FRSQRTS] = 7;
   instructionLatencies[ARM64_INS_REV16] = 3;
   instructionLatencies[ARM64_INS_REV32] = 3;
   instructionLatencies[ARM64_INS_REV64] = 3;
   instructionLatencies[ARM64_INS_TBL] = 15;  // 3 x Number of regs in table (max 4?); + 3 in 128-bit
   instructionLatencies[ARM64_INS_TBX] = 15;
   instructionLatencies[ARM64_INS_UMOV] = 6;
   instructionLatencies[ARM64_INS_SMOV] = 6;
   instructionLatencies[ARM64_INS_TRN1] = 3;
   instructionLatencies[ARM64_INS_TRN2] = 3;
   instructionLatencies[ARM64_INS_UZP1] = 3;
   instructionLatencies[ARM64_INS_UZP2] = 3;
   instructionLatencies[ARM64_INS_ZIP1] = 3;
   instructionLatencies[ARM64_INS_ZIP2] = 3;
   
   // Cryptography extensions and CRC
   instructionLatencies[ARM64_INS_AESD] = 3;
   instructionLatencies[ARM64_INS_AESE] = 3;
   instructionLatencies[ARM64_INS_AESIMC] = 3;
   instructionLatencies[ARM64_INS_AESMC] = 3;
   instructionLatencies[ARM64_INS_PMULL] = 3;
   instructionLatencies[ARM64_INS_PMULL2] = 3;
   instructionLatencies[ARM64_INS_CRC32B] = 2;
   instructionLatencies[ARM64_INS_CRC32CB] = 2;
   instructionLatencies[ARM64_INS_CRC32CH] = 2;
   instructionLatencies[ARM64_INS_CRC32CW] = 2;
   instructionLatencies[ARM64_INS_CRC32CX] = 2;
   instructionLatencies[ARM64_INS_CRC32H] = 2;
   instructionLatencies[ARM64_INS_CRC32W] = 2;
   instructionLatencies[ARM64_INS_CRC32X] = 2;

   m_lll_cutoff = Sim()->getCfg()->getInt("perf_model/core/interval_timer/lll_cutoff");
}

unsigned int CoreModelCortexA72::getInstructionLatency(const MicroOp *uop) const
{
   dl::Decoder::decoder_opcode instruction_type = uop->getInstructionOpcode();
   if (instruction_type == 65535 || instruction_type == 65534)  // FIXME Capstone library bug
     return 1;
   LOG_ASSERT_ERROR(instruction_type < ARM64_INS_ENDING, "Invalid instruction type %d", instruction_type);
   return instructionLatencies[instruction_type];
}

unsigned int CoreModelCortexA72::getAluLatency(const MicroOp *uop) const
{
   switch(uop->getInstructionOpcode()) {
      case ARM64_INS_SDIV:
      case ARM64_INS_UDIV:
         if (uop->getOperandSize() > 32)
            return 4;  // Approximate, data-dependent  (4 -- 20)
         else
            return 4;  // Approximate, data-dependent (4 -- 12)
      case ARM64_INS_MADD:
      case ARM64_INS_MSUB:
         if (uop->getOperandSize() > 32)
            return 5;  // but sequence of instrs can issue every 3 cycles...
         else
            return 3;     
      default:
         return getInstructionLatency(uop);
         //LOG_PRINT_ERROR("Don't know the ALU latency for this MicroOp.");
   }
}

unsigned int CoreModelCortexA72::getBypassLatency(const DynamicMicroOp *uop) const
{
   const DynamicMicroOpCortexA72 *info = uop->getCoreSpecificInfo<DynamicMicroOpCortexA72>();
   DynamicMicroOpCortexA72::uop_bypass_t bypass_type = info->getBypassType();
   LOG_ASSERT_ERROR(bypass_type >=0 && bypass_type < DynamicMicroOpCortexA72::UOP_BYPASS_SIZE, "Invalid bypass type %d", bypass_type);
   return bypassLatencies[bypass_type];
}

unsigned int CoreModelCortexA72::getLongestLatency() const
{
   return 36;  // ASIMD FDIV F64
}

IntervalContention* CoreModelCortexA72::createIntervalContentionModel(const Core *core) const
{
   return new IntervalContentionCortexA72(core, this);
}

RobContention* CoreModelCortexA72::createRobContentionModel(const Core *core) const
{
   return new RobContentionCortexA72(core, this);
}

DynamicMicroOp* CoreModelCortexA72::createDynamicMicroOp(Allocator *alloc, const MicroOp *uop, ComponentPeriod period) const
{
   DynamicMicroOpCortexA72 *info = DynamicMicroOp::alloc<DynamicMicroOpCortexA72>(alloc, uop, this, period);
   info->uop_port = DynamicMicroOpCortexA72::getPort(uop);
   info->uop_bypass = DynamicMicroOpCortexA72::getBypassType(uop);
   info->uop_alu = DynamicMicroOpCortexA72::getAlu(uop);
   return info;
}

#endif /* SNIPER_ARM */
