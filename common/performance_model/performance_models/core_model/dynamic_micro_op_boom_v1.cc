#include "dynamic_micro_op_boom_v1.h"
#include "micro_op.h"

#include <riscv_meta.h>

const char* DynamicMicroOpBoomV1::getType() const
{
   return "boomv1";
}

const char* DynamicMicroOpBoomV1::PortTypeString(DynamicMicroOpBoomV1::uop_port_t port)
{
   switch(port)
   {
      case UOP_PORT0:         return "port0";
      case UOP_PORT1:         return "port1";
      case UOP_PORT2:         return "port2";
      case UOP_PORT012:       return "port012";
      default:
         LOG_PRINT_ERROR("Unknown port type %d", port);
   }
}

DynamicMicroOpBoomV1::uop_port_t DynamicMicroOpBoomV1::getPort(const MicroOp *uop)
{
      if(instrlist[uop->getInstructionOpcode()].has_fpu || instrlist[uop->getInstructionOpcode()].has_fdiv || instrlist[uop->getInstructionOpcode()].has_mul ) {
            return DynamicMicroOpBoomV1::UOP_PORT0;
      } else if(instrlist[uop->getInstructionOpcode()].has_div ) {
            return DynamicMicroOpBoomV1::UOP_PORT1;
      } else if(instrlist[uop->getInstructionOpcode()].is_memory) {
            return DynamicMicroOpBoomV1::UOP_PORT2;
      } else {
            return DynamicMicroOpBoomV1::UOP_PORT012;
      }
}

DynamicMicroOpBoomV1::uop_bypass_t DynamicMicroOpBoomV1::getBypassType(const MicroOp *uop)
{
   switch(uop->getSubtype())
   {
      case MicroOp::UOP_SUBTYPE_LOAD:
         if (uop->isFpLoadStore())
            return UOP_BYPASS_LOAD_FP;
         break;
      case MicroOp::UOP_SUBTYPE_STORE:
         if (uop->isFpLoadStore())
           return UOP_BYPASS_FP_STORE;
         break;
      default:
         break;
   }
   return UOP_BYPASS_NONE;
}

DynamicMicroOpBoomV1::uop_alu_t DynamicMicroOpBoomV1::getAlu(const MicroOp *uop)
{
   switch(uop->uop_type)
   {
      case MicroOp::UOP_EXECUTE:
         switch(uop->getInstructionOpcode())
         {
            case rv_op_div:
            case rv_op_divu:
            case rv_op_divw:
            case rv_op_divuw:
            case rv_op_divd:
            case rv_op_divud:
            case rv_op_fdiv_s:
            case rv_op_fdiv_d:
            case rv_op_fdiv_q:
            case rv_op_fsqrt_s:
            case rv_op_fsqrt_d:
            case rv_op_fsqrt_q:
               return UOP_ALU_TRIG;
            default:
               return UOP_ALU_NONE;
         }
      default:
         return UOP_ALU_NONE;
   }
}

DynamicMicroOpBoomV1::DynamicMicroOpBoomV1(const MicroOp *uop, const CoreModel *core_model, ComponentPeriod period)
   : DynamicMicroOp(uop, core_model, period)
   , uop_port(DynamicMicroOpBoomV1::getPort(uop))
   , uop_alu(DynamicMicroOpBoomV1::getAlu(uop))
   , uop_bypass(DynamicMicroOpBoomV1::getBypassType(uop))
{
}
