#include "core_model_cortex_a53.h"
#include "interval_contention_cortex_a53.h"
#include "rob_contention_cortex_a53.h"
#include "dynamic_micro_op_cortex_a53.h"
#include "log.h"
#include "config.hpp"
#include "simulator.h"

#if SNIPER_ARM == 0

#else /* SNIPER_ARM == 1 */

#include <capstone.h>
#include <decoder.h>

static unsigned int instructionLatencies[ARM64_INS_ENDING];
static unsigned int bypassLatencies[DynamicMicroOpCortexA53::UOP_BYPASS_SIZE];

// Cortex A53 latencies
// Extracted from https://hardwarebug.org/2014/05/15/cortex-a7-instruction-cycle-timings/ and
// the Cortex A55 software optimization guide

CoreModelCortexA53::CoreModelCortexA53()
{
    // Default instruction latency is one cycle
    for (unsigned int i = 0 ; i < ARM64_INS_ENDING ; i++)
    {
        instructionLatencies[i] = 1;
    }

    // TODO: are LD and ST latencies relevant here?

    // Divide and multiply instructions
    instructionLatencies[ARM64_INS_SDIV] = 3; // 6-22
    instructionLatencies[ARM64_INS_UDIV] = 3; // 5-21
    instructionLatencies[ARM64_INS_MADD] = 3;
    instructionLatencies[ARM64_INS_MSUB] = 3;
    instructionLatencies[ARM64_INS_SMADDL] = 3;
    instructionLatencies[ARM64_INS_SMSUBL] = 3;
    instructionLatencies[ARM64_INS_UMADDL] = 3;
    instructionLatencies[ARM64_INS_UMSUBL] = 3;
    //instructionLatencies[ARM64_INS_SMULH] = 9; // Not found (latency from the A53)
    //instructionLatencies[ARM64_INS_UMULH] = 9; // "

    // Miscellaneous data-processing instructions
    instructionLatencies[ARM64_INS_EXTR] = 3;  // For 2 registers on the A53 (because it has the same latency for 1 register on the A53)
    instructionLatencies[ARM64_INS_BFM] = 2;
    instructionLatencies[ARM64_INS_SBFM] = 2;
    instructionLatencies[ARM64_INS_SBFX] = 2;
    instructionLatencies[ARM64_INS_UBFIZ] = 2;
    instructionLatencies[ARM64_INS_UBFM] = 2;

    // FP data processing instructions
    instructionLatencies[ARM64_INS_FABS] = 4;
    instructionLatencies[ARM64_INS_FADD] = 4;
    instructionLatencies[ARM64_INS_FSUB] = 4;
    //   instructionLatencies[ARM64_INS_FCCMP] = 3; // Not found
    //   instructionLatencies[ARM64_INS_FCCMPE] = 3; // "
    //   instructionLatencies[ARM64_INS_FCMP] = 3; // "
    //   instructionLatencies[ARM64_INS_FCMPE] = 3; // "
    instructionLatencies[ARM64_INS_FDIV] = 18;  // 32-bit: 18; 64-bit: 32;
    //   instructionLatencies[ARM64_INS_FMIN] = 3; //Not found
    //   instructionLatencies[ARM64_INS_FMAX] = 3; // "
    //   instructionLatencies[ARM64_INS_FMINNM] = 3; // "
    //   instructionLatencies[ARM64_INS_FMAXNM] = 3; // "
    instructionLatencies[ARM64_INS_FMUL] = 6; // 32-bit: 4; 64-bit: 7;
    instructionLatencies[ARM64_INS_FNMUL] = 7;
    instructionLatencies[ARM64_INS_FMADD] = 8; // 32-bit: 8; 64-bit: 11;
    instructionLatencies[ARM64_INS_FMSUB] = 8;
    instructionLatencies[ARM64_INS_FNMADD] = 8;
    instructionLatencies[ARM64_INS_FNMSUB] = 8;
    instructionLatencies[ARM64_INS_FNEG] = 4;
    //   instructionLatencies[ARM64_INS_FRINTA] = 3; // Not found
    //   instructionLatencies[ARM64_INS_FRINTI] = 3; // "
    //   instructionLatencies[ARM64_INS_FRINTM] = 3; // "
    //   instructionLatencies[ARM64_INS_FRINTN] = 3; //
    //   instructionLatencies[ARM64_INS_FRINTP] = 3; // "
    //   instructionLatencies[ARM64_INS_FRINTX] = 3; // "
    //   instructionLatencies[ARM64_INS_FRINTZ] = 3; // "
    //   instructionLatencies[ARM64_INS_FCSEL] = 3; // "
    instructionLatencies[ARM64_INS_FSQRT] = 17;  // 32-bit: 17; 64-bit: 31

    // FP miscellaneous instructions
       instructionLatencies[ARM64_INS_FCVT] = 4; // Not found
    //   instructionLatencies[ARM64_INS_FCVTXN] = 3; // "
    //   instructionLatencies[ARM64_INS_SCVTF] = 8; // "
    //   instructionLatencies[ARM64_INS_UCVTF] = 8; // "
    //   instructionLatencies[ARM64_INS_FCVTAS] = 8; // "
    //   instructionLatencies[ARM64_INS_FCVTAU] = 8; // "
    //   instructionLatencies[ARM64_INS_FCVTMS] = 8; // "
    //   instructionLatencies[ARM64_INS_FCVTMU] = 8; // "
    //   instructionLatencies[ARM64_INS_FCVTNS] = 8; // "
    //   instructionLatencies[ARM64_INS_FCVTNU] = 8; // "
    //   instructionLatencies[ARM64_INS_FCVTPS] = 8; // "
    //   instructionLatencies[ARM64_INS_FCVTPU] = 8; // "
    //   instructionLatencies[ARM64_INS_FCVTZS] = 8; // "
    //   instructionLatencies[ARM64_INS_FCVTZU] = 8; // "
    instructionLatencies[ARM64_INS_FMOV] = 4;

    // ASIMD integer instructions
    //   instructionLatencies[ARM64_INS_SABD] = 3; // Not found
    //   instructionLatencies[ARM64_INS_UABD] = 3; // "
    //   instructionLatencies[ARM64_INS_SABA] = 5;  // "
    //   instructionLatencies[ARM64_INS_UABA] = 5;  // "
    //   instructionLatencies[ARM64_INS_SABAL] = 4; // "
    //   instructionLatencies[ARM64_INS_SABAL2] = 4; // "
    //   instructionLatencies[ARM64_INS_UABAL] = 4; // "
    //   instructionLatencies[ARM64_INS_UABAL2] = 4; // "
    //   instructionLatencies[ARM64_INS_SABDL] = 3; // "
    //   instructionLatencies[ARM64_INS_UABDL] = 3; // "
    instructionLatencies[ARM64_INS_ABS] = 4;
    instructionLatencies[ARM64_INS_ADDP] = 4;
    instructionLatencies[ARM64_INS_NEG] = 4;
    instructionLatencies[ARM64_INS_SADDL] = 4;
    instructionLatencies[ARM64_INS_SADDL2] = 4;
    instructionLatencies[ARM64_INS_SADDLP] = 4;
    instructionLatencies[ARM64_INS_SADDW] = 4;
    instructionLatencies[ARM64_INS_SADDW2] = 4;
    instructionLatencies[ARM64_INS_SHADD] = 4;
    instructionLatencies[ARM64_INS_SHSUB] = 4;
    instructionLatencies[ARM64_INS_SSUBL] = 4;
    instructionLatencies[ARM64_INS_SSUBL2] = 4;
    instructionLatencies[ARM64_INS_SSUBW] = 4;
    instructionLatencies[ARM64_INS_SSUBW2] = 4;
    instructionLatencies[ARM64_INS_UADDL] = 4;
    instructionLatencies[ARM64_INS_UADDL2] = 4;
    instructionLatencies[ARM64_INS_UADDLP] = 4;
    instructionLatencies[ARM64_INS_UADDW] = 4;
    instructionLatencies[ARM64_INS_UADDW2] = 4;
    instructionLatencies[ARM64_INS_UHADD] = 4;
    instructionLatencies[ARM64_INS_UHSUB] = 4;
    instructionLatencies[ARM64_INS_USUBW] = 4;
    instructionLatencies[ARM64_INS_USUBW2] = 4;
    instructionLatencies[ARM64_INS_ADDHN] = 4;
    instructionLatencies[ARM64_INS_ADDHN2] = 4;
    instructionLatencies[ARM64_INS_RADDHN] = 4;
    instructionLatencies[ARM64_INS_RADDHN2] = 4;
    instructionLatencies[ARM64_INS_RSUBHN] = 4;
    instructionLatencies[ARM64_INS_RSUBHN2] = 4;
    instructionLatencies[ARM64_INS_SQABS] = 4;
    instructionLatencies[ARM64_INS_SQADD] = 4;
    instructionLatencies[ARM64_INS_SQNEG] = 4;
    instructionLatencies[ARM64_INS_SQSUB] = 4;
    instructionLatencies[ARM64_INS_SRHADD] = 4;
    instructionLatencies[ARM64_INS_SUBHN] = 4;
    instructionLatencies[ARM64_INS_SUBHN2] = 4;
    instructionLatencies[ARM64_INS_SUQADD] = 4;
    instructionLatencies[ARM64_INS_UQADD] = 4;
    instructionLatencies[ARM64_INS_UQSUB] = 4;
    instructionLatencies[ARM64_INS_URHADD] = 4;
    instructionLatencies[ARM64_INS_USQADD] = 4;
    instructionLatencies[ARM64_INS_ADDV] = 3;
    //   instructionLatencies[ARM64_INS_SADDLV] = 6;  // Not found
    //   instructionLatencies[ARM64_INS_UADDLV] = 6;  // "
    //   instructionLatencies[ARM64_INS_CMEQ] = 3; // "
    //   instructionLatencies[ARM64_INS_CMGE] = 3; // "
    //   instructionLatencies[ARM64_INS_CMGT] = 3; // "
    //   instructionLatencies[ARM64_INS_CMHI] = 3; // "
    //   instructionLatencies[ARM64_INS_CMHS] = 3; // "
    //   instructionLatencies[ARM64_INS_CMLE] = 3; // "
    //   instructionLatencies[ARM64_INS_CMLT] = 3; // "
    //   instructionLatencies[ARM64_INS_CMTST] = 3; // "

    //   instructionLatencies[ARM64_INS_SMAX] = 3; // "
    //   instructionLatencies[ARM64_INS_SMAXP] = 3; // "
    //   instructionLatencies[ARM64_INS_SMIN] = 3; // "
    //   instructionLatencies[ARM64_INS_SMINP] = 3; // "
    //   instructionLatencies[ARM64_INS_UMAX] = 3; // "
    //   instructionLatencies[ARM64_INS_UMAXP] = 3; // "
    //   instructionLatencies[ARM64_INS_UMIN] = 3; // "
    //   instructionLatencies[ARM64_INS_UMINP] = 3; // "
    //   instructionLatencies[ARM64_INS_SMAXV] = 6; // "
    //   instructionLatencies[ARM64_INS_SMINV] = 6; // "
    //   instructionLatencies[ARM64_INS_UMAXV] = 6; // "
    //   instructionLatencies[ARM64_INS_UMINV] = 6; // "
    instructionLatencies[ARM64_INS_MUL] = 3;
    //   instructionLatencies[ARM64_INS_PMUL] = 5; // "
    //   instructionLatencies[ARM64_INS_SQDMULH] = 5; // "
    //   instructionLatencies[ARM64_INS_SQRDMULH] = 5; // "

    instructionLatencies[ARM64_INS_MLA] = 11;
    instructionLatencies[ARM64_INS_MLS] = 11;
    instructionLatencies[ARM64_INS_SMLAL] = 4;
    instructionLatencies[ARM64_INS_SMLAL2] = 4;
    instructionLatencies[ARM64_INS_SMLSL] = 4;
    instructionLatencies[ARM64_INS_SMLSL2] = 4;
    instructionLatencies[ARM64_INS_UMLAL] = 4;
    instructionLatencies[ARM64_INS_UMLAL2] = 4;
    instructionLatencies[ARM64_INS_UMLSL] = 4;
    instructionLatencies[ARM64_INS_UMLSL2] = 4;
    //   instructionLatencies[ARM64_INS_SQDMLAL] = 4; // Not found
    //   instructionLatencies[ARM64_INS_SQDMLAL2] = 4; // "
    //   instructionLatencies[ARM64_INS_SQDMLSL] = 4; // "
    //   instructionLatencies[ARM64_INS_SQDMLSL2] = 4; // "
    instructionLatencies[ARM64_INS_SMULL] = 4;
    instructionLatencies[ARM64_INS_SMULL2] = 4;
    instructionLatencies[ARM64_INS_UMULL] = 4;
    instructionLatencies[ARM64_INS_UMULL2] = 4;
    instructionLatencies[ARM64_INS_SQDMULL] = 4;
    instructionLatencies[ARM64_INS_SQDMULL2] = 4;
    //   instructionLatencies[ARM64_INS_PMULL] = 4; // Not found
    //   instructionLatencies[ARM64_INS_PMULL2] = 4; // "
    //   instructionLatencies[ARM64_INS_SADALP] = 4; // "
    //   instructionLatencies[ARM64_INS_UADALP] = 4; // "
    //   instructionLatencies[ARM64_INS_SRA] = 4;
    //   instructionLatencies[ARM64_INS_SRSRA] = 4; // "
    //   instructionLatencies[ARM64_INS_USRA] = 4; // "
    //   instructionLatencies[ARM64_INS_URSRA] = 4; // "
    //   instructionLatencies[ARM64_INS_SHL] = 3; // "
    //   instructionLatencies[ARM64_INS_SHLL] = 3; // "
    //   instructionLatencies[ARM64_INS_SHLL2] = 3; // "
    //   instructionLatencies[ARM64_INS_SHRN] = 3; // "
    //   instructionLatencies[ARM64_INS_SHRN2] = 3; // "
    //   instructionLatencies[ARM64_INS_SLI] = 4;   // Not found
    //   instructionLatencies[ARM64_INS_SRI] = 4;   // "
    //   instructionLatencies[ARM64_INS_SSHLL] = 3; // "
    //   instructionLatencies[ARM64_INS_SSHLL2] = 3; // "
    //   instructionLatencies[ARM64_INS_SSHR] = 3; // "
    //   instructionLatencies[ARM64_INS_SXTL] = 3;
    //   instructionLatencies[ARM64_INS_SXTL2] = 3;
    //   instructionLatencies[ARM64_INS_USHLL] = 3; // "
    //   instructionLatencies[ARM64_INS_USHLL2] = 3; // "
    //   instructionLatencies[ARM64_INS_USHR] = 3; // "
    //   instructionLatencies[ARM64_INS_UXTL] = 3;
    //   instructionLatencies[ARM64_INS_UXTL2] = 3;
    //   instructionLatencies[ARM64_INS_RSHRN] = 4; // "
    //   instructionLatencies[ARM64_INS_RSHRN2] = 4; // "
    //   instructionLatencies[ARM64_INS_SRSHR] = 4; // "
    //   instructionLatencies[ARM64_INS_SQSHL] = 4; // "
    //   instructionLatencies[ARM64_INS_SQSHLU] = 4; // "
    //   instructionLatencies[ARM64_INS_SQRSHRN] = 4; // "
    //   instructionLatencies[ARM64_INS_SQRSHRN2] = 4; // "
    //   instructionLatencies[ARM64_INS_SQRSHRUN] = 4; // "
    //   instructionLatencies[ARM64_INS_SQRSHRUN2] = 4; // "
    //   instructionLatencies[ARM64_INS_SQSHRN] = 4; // "
    //   instructionLatencies[ARM64_INS_SQSHRN2] = 4; // "
    //   instructionLatencies[ARM64_INS_SQSHRUN] = 4; // "
    //   instructionLatencies[ARM64_INS_SQSHRUN2] = 4; // "
    //   instructionLatencies[ARM64_INS_URSHR] = 4; // "
    //   instructionLatencies[ARM64_INS_UQSHL] = 4; // "
    //   instructionLatencies[ARM64_INS_UQRSHRN] = 4; // "
    //   instructionLatencies[ARM64_INS_UQRSHRN2] = 4; // "
    //   instructionLatencies[ARM64_INS_UQSHRN] = 4; // "
    //   instructionLatencies[ARM64_INS_UQSHRN2] = 4; // "
    //   instructionLatencies[ARM64_INS_SSHL] = 4;   // "
    //   instructionLatencies[ARM64_INS_USHL] = 4;   // "
    //   instructionLatencies[ARM64_INS_SRSHL] = 5;  // "
    //   instructionLatencies[ARM64_INS_SQRSHL] = 5; // "
    //   instructionLatencies[ARM64_INS_SQSHL] = 5; // "
    //   instructionLatencies[ARM64_INS_URSHL] = 5; // "
    //   instructionLatencies[ARM64_INS_UQRSHL] = 5; // "
    //   instructionLatencies[ARM64_INS_UQSHL] = 5; // "

    // ASIMD floating-point instructions
    instructionLatencies[ARM64_INS_FABD] = 4;
    //   instructionLatencies[ARM64_INS_FADDP] = 7; // "
    //   instructionLatencies[ARM64_INS_FACGE] = 3; // "
    //   instructionLatencies[ARM64_INS_FACGT] = 3; // "
    //   instructionLatencies[ARM64_INS_FCMEQ] = 3; // "
    //   instructionLatencies[ARM64_INS_FCMGE] = 3; // "
    //   instructionLatencies[ARM64_INS_FCMGT] = 3; // "
    //   instructionLatencies[ARM64_INS_FCMLE] = 3; // "
    //   instructionLatencies[ARM64_INS_FCMLT] = 3; // "
    instructionLatencies[ARM64_INS_FCVT] = 4;
    instructionLatencies[ARM64_INS_FCVTL] = 4;
    //   instructionLatencies[ARM64_INS_FCVTL2] = 7;  // "
    instructionLatencies[ARM64_INS_FCVTN] = 4;
    instructionLatencies[ARM64_INS_FCVTN2] = 4;
    //   instructionLatencies[ARM64_INS_FCVTXN] = 7;   // "
    //   instructionLatencies[ARM64_INS_FCVTXN2] = 7;  // "
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
    //   instructionLatencies[ARM64_INS_FMINP] = 3; // "
    //   instructionLatencies[ARM64_INS_FMAXP] = 3; // "
    //   instructionLatencies[ARM64_INS_FMINNMP] = 3; // "
    //   instructionLatencies[ARM64_INS_FMAXNMP] = 3; // "
    //   instructionLatencies[ARM64_INS_FMINV] = 6; // "
    //   instructionLatencies[ARM64_INS_FMAXV] = 6; // "
    //   instructionLatencies[ARM64_INS_FMINNMV] = 6; // "
    //   instructionLatencies[ARM64_INS_FMAXNMV] = 6; // "
    //   instructionLatencies[ARM64_INS_FMULX] = 4; // "
    instructionLatencies[ARM64_INS_FMLA] = 8;
    instructionLatencies[ARM64_INS_FMLS] = 8;
    instructionLatencies[ARM64_INS_FNEG] = 3;
    //   instructionLatencies[ARM64_INS_FRINTA] = 4; // "
    //   instructionLatencies[ARM64_INS_FRINTI] = 4; // "
    //   instructionLatencies[ARM64_INS_FRINTM] = 4; // "
    //   instructionLatencies[ARM64_INS_FRINTN] = 4; // "
    //   instructionLatencies[ARM64_INS_FRINTP] = 4; // "
    //   instructionLatencies[ARM64_INS_FRINTX] = 4; // "
    //   instructionLatencies[ARM64_INS_FRINTZ] = 4; // "

    // ASIMD miscellaneous instructions
    //   instructionLatencies[ARM64_INS_RBIT] = 3; // "
    //   instructionLatencies[ARM64_INS_BIF] = 3; // "
    //   instructionLatencies[ARM64_INS_BIT] = 3; // "
    //   instructionLatencies[ARM64_INS_BSL] = 3; // "
    //   instructionLatencies[ARM64_INS_CLS] = 3; // "
    //   instructionLatencies[ARM64_INS_CLZ] = 3; // "
    //   instructionLatencies[ARM64_INS_CNT] = 3; // "
    //   instructionLatencies[ARM64_INS_DUP] = 8; // "
    instructionLatencies[ARM64_INS_EXT] = 5; // 64-bit: 4; 128-bit:5
    //   instructionLatencies[ARM64_INS_XTN] = 3; // Not found
    //   instructionLatencies[ARM64_INS_SQXTN] = 4; // "
    //   instructionLatencies[ARM64_INS_SQXTN2] = 4; // "
    //   instructionLatencies[ARM64_INS_SQXTUN] = 4; // "
    //   instructionLatencies[ARM64_INS_SQXTUN2] = 4; // "
    //   instructionLatencies[ARM64_INS_UQXTN] = 4; // "
    //   instructionLatencies[ARM64_INS_UQXTN2] = 4; // "
    //   instructionLatencies[ARM64_INS_INS] = 3; // "
    instructionLatencies[ARM64_INS_MOVI] = 1;
    //   instructionLatencies[ARM64_INS_FRECPE] = 4; // "
    //   instructionLatencies[ARM64_INS_FRECPX] = 4; // "
    //   instructionLatencies[ARM64_INS_FRSQRTE] = 4; // "
    //   instructionLatencies[ARM64_INS_URECPE] = 4; // "
    //   instructionLatencies[ARM64_INS_URSQRTE] = 4; // "
    //   instructionLatencies[ARM64_INS_FRECPS] = 7; // "
    //   instructionLatencies[ARM64_INS_FRSQRTS] = 7; // "
    //   instructionLatencies[ARM64_INS_REV16] = 3; // "
    //   instructionLatencies[ARM64_INS_REV32] = 3; // "
    //   instructionLatencies[ARM64_INS_REV64] = 3; // "
    instructionLatencies[ARM64_INS_TBL] = 5;  // 1 or 2 reg: 4; 3 or 4 reg: 5
    //   instructionLatencies[ARM64_INS_TBX] = 15; // "
    //   instructionLatencies[ARM64_INS_UMOV] = 6; // Not found
    //   instructionLatencies[ARM64_INS_SMOV] = 6; // "
    instructionLatencies[ARM64_INS_TRN1] = 5;
    instructionLatencies[ARM64_INS_TRN2] = 5;
    instructionLatencies[ARM64_INS_UZP1] = 6; // 64-bit: 5; 128-bit: 6
    instructionLatencies[ARM64_INS_UZP2] = 6; // "
    instructionLatencies[ARM64_INS_ZIP1] = 6; // "
    instructionLatencies[ARM64_INS_ZIP2] = 6; // "
    instructionLatencies[ARM64_INS_LDR] = 3;
    instructionLatencies[ARM64_INS_LDP] = 3;
    instructionLatencies[ARM64_INS_LDRB] = 3;

    // Cryptography extensions and CRC
    //   instructionLatencies[ARM64_INS_AESD] = 3; // Not found
    //   instructionLatencies[ARM64_INS_AESE] = 3; // "
    //   instructionLatencies[ARM64_INS_AESIMC] = 3; // "
    //   instructionLatencies[ARM64_INS_AESMC] = 3; // "
    //   instructionLatencies[ARM64_INS_PMULL] = 3; // "
    //   instructionLatencies[ARM64_INS_PMULL2] = 3; // "
    //   instructionLatencies[ARM64_INS_CRC32B] = 2; // "
    //   instructionLatencies[ARM64_INS_CRC32CB] = 2; // "
    //   instructionLatencies[ARM64_INS_CRC32CH] = 2; // "
    //   instructionLatencies[ARM64_INS_CRC32CW] = 2; // "
    //   instructionLatencies[ARM64_INS_CRC32CX] = 2; // "
    //   instructionLatencies[ARM64_INS_CRC32H] = 2; // "
    //   instructionLatencies[ARM64_INS_CRC32W] = 2; // "
    //   instructionLatencies[ARM64_INS_CRC32X] = 2; // "

    m_lll_cutoff = Sim()->getCfg()->getInt("perf_model/core/interval_timer/lll_cutoff");
}

unsigned int CoreModelCortexA53::getInstructionLatency(const MicroOp *uop) const
{
    dl::Decoder::decoder_opcode instruction_type = uop->getInstructionOpcode();
    if (instruction_type == 65535 || instruction_type == 65534)  // FIXME Capstone library bug
        return 1;
    LOG_ASSERT_ERROR(instruction_type < ARM64_INS_ENDING, "Invalid instruction type %d", instruction_type);
    switch(instruction_type) {
    case ARM64_INS_ADD:
    case ARM64_INS_SUB:
       if (uop->getOperandSize() > 64)
          return 2;
    /*case ARM64_INS_AND:
    case ARM64_INS_BIC:
    case ARM64_INS_EON:
    case ARM64_INS_EOR:
    case ARM64_INS_ORN:
    case ARM64_INS_ORR:
    case ARM64_INS_CMP:
        if (uop->getDecodedInstruction()->has_modifiers())
            return 2;*/
    }
    return instructionLatencies[instruction_type];
}

unsigned int CoreModelCortexA53::getAluLatency(const MicroOp *uop) const
{
    switch(uop->getInstructionOpcode()) {
    case ARM64_INS_SDIV:
        return 3; //12;  // Approximate, data-dependent (4 -- 20)
    case ARM64_INS_UDIV:
        return 3; //11;  // Approximate, data-dependent (3 -- 19)
    case ARM64_INS_LDR:
    case ARM64_INS_LDP:
        if (uop->getOperandSize() > 64)
            return 2;
        else
            return 0;
    /*case ARM64_INS_FMUL:
    case ARM64_INS_FNMUL:
    case ARM64_INS_FMADD:
        if (uop->getOperandSize() > 32)
            return 4;
        else
            return 0;*/
    case ARM64_INS_FMLA:
        if (uop->getOperandSize() > 32)
            return 5;
        else
            return 0;
    case ARM64_INS_FDIV:
        if (uop->getOperandSize() > 32)
            return 29;
        else
            return 15;
    case ARM64_INS_FSQRT:
        if (uop->getOperandSize() > 32)
            return 28;
        else
            return 14;
    case ARM64_INS_MUL:
    case ARM64_INS_FADD:
    case ARM64_INS_TRN1:
    case ARM64_INS_TRN2:
    case ARM64_INS_UZP1:
    case ARM64_INS_UZP2:
        if (uop->getOperandSize() > 64)
            return 4;
        else if (uop->getOperandSize() > 32)
            return 2;
        else
            return 0;
    case ARM64_INS_EXT:
    //case ARM64_INS_ADD:
        if (uop->getOperandSize() > 64)
            return 2;
        else
            return 0;
    case ARM64_INS_UADDL:
    case ARM64_INS_UADDL2:
    case ARM64_INS_SADDL:
    case ARM64_INS_SADDL2:
    case ARM64_INS_UMULL:
    case ARM64_INS_UMULL2:
    case ARM64_INS_SMULL:
    case ARM64_INS_SMULL2:
    case ARM64_INS_MLA: // The latency can be two or four depending on the operand type
    case ARM64_INS_TBL: // 1 or 2 reg: 1; 3 or 4 reg: 2
        return 2;
    default:
        return 0;
        //return getInstructionLatency(uop);
        //LOG_PRINT_ERROR("Don't know the ALU latency for this MicroOp.");
    }
}

unsigned int CoreModelCortexA53::getBypassLatency(const DynamicMicroOp *uop) const
{
    const DynamicMicroOpCortexA53 *info = uop->getCoreSpecificInfo<DynamicMicroOpCortexA53>();
    DynamicMicroOpCortexA53::uop_bypass_t bypass_type = info->getBypassType();
    LOG_ASSERT_ERROR(bypass_type >=0 && bypass_type < DynamicMicroOpCortexA53::UOP_BYPASS_SIZE, "Invalid bypass type %d", bypass_type);
    return bypassLatencies[bypass_type];
}

unsigned int CoreModelCortexA53::getLongestLatency() const
{
    return 32;  // ASIMD FDIV F64
}

IntervalContention* CoreModelCortexA53::createIntervalContentionModel(const Core *core) const
{
    return new IntervalContentionCortexA53(core, this);
}

RobContention* CoreModelCortexA53::createRobContentionModel(const Core *core) const
{
    return new RobContentionCortexA53(core, this);
}

DynamicMicroOp* CoreModelCortexA53::createDynamicMicroOp(Allocator *alloc, const MicroOp *uop, ComponentPeriod period) const
{
    DynamicMicroOpCortexA53 *info = DynamicMicroOp::alloc<DynamicMicroOpCortexA53>(alloc, uop, this, period);
    info->uop_port = DynamicMicroOpCortexA53::getPort(uop);
    info->uop_bypass = DynamicMicroOpCortexA53::getBypassType(uop);
    info->uop_alu = DynamicMicroOpCortexA53::getAlu(uop);
    return info;
}

#endif /* SNIPER_ARM */
