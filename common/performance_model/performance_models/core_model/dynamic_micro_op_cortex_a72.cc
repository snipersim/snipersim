#include "dynamic_micro_op_cortex_a72.h"
#include "micro_op.h"

#if SNIPER_ARM == 0

#else /* SNIPER_ARM == 1 */

#include <capstone.h>

const char* DynamicMicroOpCortexA72::getType() const
{
   return "cortex-a72";
}

const char* DynamicMicroOpCortexA72::PortTypeString(DynamicMicroOpCortexA72::uop_port_t port)
{
   switch(port)
   {
      case UOP_PORT0:   return "port0";
      case UOP_PORT0_12:   return "port0_12";
      case UOP_PORT12:   return "port12";
      case UOP_PORT3:   return "port3";
      case UOP_PORT12_3:   return "port12_3";
      case UOP_PORT4:   return "port4";
      case UOP_PORT5:  return "port5";
      case UOP_PORT6: return "port6";
      case UOP_PORT7: return "port7";
      case UOP_PORT6_12: return "port6_12";
      case UOP_PORT7_12: return "port7_12";
      default:
         LOG_PRINT_ERROR("Unknown port type %d", port);
   }
}

DynamicMicroOpCortexA72::uop_port_t DynamicMicroOpCortexA72::getPort(const MicroOp *uop)
{
   switch(uop->uop_subtype) {
      case MicroOp::UOP_SUBTYPE_FP_ADDSUB:
         return DynamicMicroOpCortexA72::UOP_PORT4;  // FIXME This is a rough simplification; could be more precise, 
      case MicroOp::UOP_SUBTYPE_FP_MULDIV:           //        sending instructions to their corresponding port, as below
         return DynamicMicroOpCortexA72::UOP_PORT5;  //        but right now Dynamorio cannot send us some SIMD instructions...
      case MicroOp::UOP_SUBTYPE_LOAD: 
         if (uop->isWriteback()) {
            return DynamicMicroOpCortexA72::UOP_PORT6_12;
         } else {
            return DynamicMicroOpCortexA72::UOP_PORT6;
         }
      case MicroOp::UOP_SUBTYPE_STORE: 
        if (uop->isWriteback()) {
            return DynamicMicroOpCortexA72::UOP_PORT7_12;
        } else {
            return DynamicMicroOpCortexA72::UOP_PORT7;
        }
      case MicroOp::UOP_SUBTYPE_GENERIC:
         switch(uop->getInstructionOpcode()) {
            case ARM64_INS_ADD: 
            case ARM64_INS_AND: 
            case ARM64_INS_BIC: 
            case ARM64_INS_EON: 
            case ARM64_INS_EOR: 
            case ARM64_INS_ORN: 
            case ARM64_INS_ORR: 
            case ARM64_INS_SUB:
            //case ARM64_INS_CMP:
               return uop->getDecodedInstruction()->has_modifiers() ?  // Instructions with shifters and extenders
                      DynamicMicroOpCortexA72::UOP_PORT3 : 
                      DynamicMicroOpCortexA72::UOP_PORT12;
            case ARM64_INS_BL:
            case ARM64_INS_BLR:
               return DynamicMicroOpCortexA72::UOP_PORT0_12;
            case ARM64_INS_SDIV:
            case ARM64_INS_UDIV:
            case ARM64_INS_MADD:
            case ARM64_INS_MUL:
            case ARM64_INS_MSUB:
            case ARM64_INS_SMADDL:
            case ARM64_INS_SMSUBL:
            case ARM64_INS_UMADDL:
            case ARM64_INS_UMSUBL:
            case ARM64_INS_SMULH:
            case ARM64_INS_UMULH:
            case ARM64_INS_BFM:
            case ARM64_INS_CRC32B:
            case ARM64_INS_CRC32CB:
            case ARM64_INS_CRC32CH:
            case ARM64_INS_CRC32CW:
            case ARM64_INS_CRC32CX:
            case ARM64_INS_CRC32H:
            case ARM64_INS_CRC32W:
            case ARM64_INS_CRC32X:
               return DynamicMicroOpCortexA72::UOP_PORT3;
//            case ARM64_INS_EXT:  // 2 registers version -- TODO? Not clear from description
//               return DynamicMicroOpCortexA72::UOP_PORT12_3;
            default:
               return DynamicMicroOpCortexA72::UOP_PORT12;  
         }
      case MicroOp::UOP_SUBTYPE_BRANCH:
         switch(uop->getInstructionOpcode()) {
            case ARM64_INS_BL:
            case ARM64_INS_BLR:
               return DynamicMicroOpCortexA72::UOP_PORT0_12;  // These branch instructions also utilize the Integer pipeline
            default:
               return DynamicMicroOpCortexA72::UOP_PORT0;
         }
      default:
         LOG_PRINT_ERROR("Unknown uop_subtype %u", uop->uop_subtype);
   }
}

DynamicMicroOpCortexA72::uop_bypass_t DynamicMicroOpCortexA72::getBypassType(const MicroOp *uop)
{
   return UOP_BYPASS_NONE;
}

DynamicMicroOpCortexA72::uop_alu_t DynamicMicroOpCortexA72::getAlu(const MicroOp *uop)
{
   switch(uop->uop_type)
   {
      default:
         return UOP_ALU_NONE;
      case MicroOp::UOP_EXECUTE:
         switch(uop->getInstructionOpcode())
         {
            case ARM64_INS_SDIV:
            case ARM64_INS_UDIV:
            case ARM64_INS_MADD:
            case ARM64_INS_MSUB:
            case ARM64_INS_SMADDL:
            case ARM64_INS_SMSUBL:
            case ARM64_INS_UMADDL:
            case ARM64_INS_UMSUBL:
            case ARM64_INS_SMULH:
            case ARM64_INS_UMULH:
            case ARM64_INS_BFM:
            case ARM64_INS_CRC32B:
            case ARM64_INS_CRC32CB:
            case ARM64_INS_CRC32CH:
            case ARM64_INS_CRC32CW:
            case ARM64_INS_CRC32CX:
            case ARM64_INS_CRC32H:
            case ARM64_INS_CRC32W:
            case ARM64_INS_CRC32X:
               return UOP_ALU_MULDIV;
            default:
               return UOP_ALU_NONE;
         }
   }
}

DynamicMicroOpCortexA72::DynamicMicroOpCortexA72(const MicroOp *uop, const CoreModel *core_model, ComponentPeriod period)
   : DynamicMicroOp(uop, core_model, period)
   , uop_port(DynamicMicroOpCortexA72::getPort(uop))
   , uop_alu(DynamicMicroOpCortexA72::getAlu(uop))
   , uop_bypass(DynamicMicroOpCortexA72::getBypassType(uop))
{
}

#endif /* SNIPER_ARM */
