#include "dynamic_micro_op_cortex_a53.h"
#include "micro_op.h"

#if SNIPER_ARM == 0

#else /* SNIPER_ARM == 1 */

#include <capstone.h>

const char* DynamicMicroOpCortexA53::getType() const
{
    return "cortex-a53";
}

const char* DynamicMicroOpCortexA53::PortTypeString(DynamicMicroOpCortexA53::uop_port_t port)
{
    switch(port)
    {
    case UOP_PORT0:       return "port0";
    case UOP_PORT0_12:    return "port0_12";
    case UOP_PORT12:      return "port12";
    case UOP_PORT5:       return "port5";
    case UOP_PORT34:      return "port34";
    case UOP_PORT3:       return "port3";
    case UOP_PORT4:       return "port4";
    case UOP_PORT5_12:    return "port5_12";
    default:
        LOG_PRINT_ERROR("Unknown port type %d", port);
    }
}

DynamicMicroOpCortexA53::uop_issue_slot_t DynamicMicroOpCortexA53::getIssueSlot(const MicroOp *uop) {
    switch(uop->getInstructionOpcode()) {
    case ARM64_INS_FCVT:
        return DynamicMicroOpCortexA53::UOP_ISSUE_SLOT_01;
    case ARM64_INS_LDP:
        return DynamicMicroOpCortexA53::UOP_ISSUE_SLOT_00;
    case ARM64_INS_ADD:
       if (uop->getOperandSize() > 64)
          return DynamicMicroOpCortexA53::UOP_ISSUE_SLOT_01;
    default:
        return DynamicMicroOpCortexA53::UOP_ISSUE_SLOT_11;
    }
}

DynamicMicroOpCortexA53::uop_port_t DynamicMicroOpCortexA53::getPort(const MicroOp *uop)
{
    switch(uop->uop_subtype) {
    case MicroOp::UOP_SUBTYPE_FP_ADDSUB:
    case MicroOp::UOP_SUBTYPE_FP_MULDIV:
        return DynamicMicroOpCortexA53::UOP_PORT34;
    case MicroOp::UOP_SUBTYPE_LOAD:
    case MicroOp::UOP_SUBTYPE_STORE:
        if (uop->isWriteback()) {
            return DynamicMicroOpCortexA53::UOP_PORT5_12;
        }
        return DynamicMicroOpCortexA53::UOP_PORT5;
    case MicroOp::UOP_SUBTYPE_GENERIC:
        switch(uop->getInstructionOpcode()) {
        case ARM64_INS_ADD:
        case ARM64_INS_SUB:
           if (uop->getOperandSize() > 64)
              return DynamicMicroOpCortexA53::UOP_PORT34;
        case ARM64_INS_AND:
        case ARM64_INS_BIC:
        case ARM64_INS_EON:
        case ARM64_INS_EOR:
        case ARM64_INS_ORN:
        case ARM64_INS_ORR:
        case ARM64_INS_CMP:
            return DynamicMicroOpCortexA53::UOP_PORT12;
        case ARM64_INS_BL:
        case ARM64_INS_BLR:
            return DynamicMicroOpCortexA53::UOP_PORT0_12;
        case ARM64_INS_SDIV:
        case ARM64_INS_UDIV:
            return DynamicMicroOpCortexA53::UOP_PORT2;
        case ARM64_INS_MADD:
        case ARM64_INS_MSUB:
        case ARM64_INS_SMADDL:
        case ARM64_INS_SMSUBL:
        case ARM64_INS_UMADDL:
        case ARM64_INS_UMSUBL:
        case ARM64_INS_SMULH:
        case ARM64_INS_UMULH:
        case ARM64_INS_MUL:
            return DynamicMicroOpCortexA53::UOP_PORT1;
        case ARM64_INS_BFM:
        case ARM64_INS_CRC32B:
        case ARM64_INS_CRC32CB:
        case ARM64_INS_CRC32CH:
        case ARM64_INS_CRC32CW:
        case ARM64_INS_CRC32CX:
        case ARM64_INS_CRC32H:
        case ARM64_INS_CRC32W:
        case ARM64_INS_CRC32X:
            return DynamicMicroOpCortexA53::UOP_PORT12;
            //            case ARM64_INS_EXT:  // 2 registers version -- TODO? Not clear from description
            //               return DynamicMicroOpCortexA53::UOP_PORT12_3;
        default:
            return DynamicMicroOpCortexA53::UOP_PORT12;
        }
    case MicroOp::UOP_SUBTYPE_BRANCH:
        switch(uop->getInstructionOpcode()) {
        case ARM64_INS_BL:
        case ARM64_INS_BLR:
            return DynamicMicroOpCortexA53::UOP_PORT0_12;  // These branch instructions also utilize the Integer pipeline
        default:
            return DynamicMicroOpCortexA53::UOP_PORT0;
        }
    default:
        LOG_PRINT_ERROR("Unknown uop_subtype %u", uop->uop_subtype);
    }
}

DynamicMicroOpCortexA53::uop_bypass_t DynamicMicroOpCortexA53::getBypassType(const MicroOp *uop)
{
    return UOP_BYPASS_NONE;
}

DynamicMicroOpCortexA53::uop_alu_t DynamicMicroOpCortexA53::getAlu(const MicroOp *uop)
{
    return UOP_ALU_NONE;
}

DynamicMicroOpCortexA53::DynamicMicroOpCortexA53(const MicroOp *uop, const CoreModel *core_model, ComponentPeriod period)
    : DynamicMicroOp(uop, core_model, period)
    , uop_port(DynamicMicroOpCortexA53::getPort(uop))
    , uop_alu(DynamicMicroOpCortexA53::getAlu(uop))
    , uop_bypass(DynamicMicroOpCortexA53::getBypassType(uop))
    , uop_issue_slot(getIssueSlot(uop))
{
}

#endif /* SNIPER_ARM */
