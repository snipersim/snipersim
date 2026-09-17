#include "dynamic_micro_op_boom_v1.h"
#include "riscv_isa.h"
#include "micro_op.h"


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
   /* By the instruction's class flags: floating-point, vector and matrix
      work on port 0, the integer divider on port 1, memory on port 2. */
   const uint32_t f = rvisa::flags_of(uop->getInstructionOpcode());
   if (f & (rvisa::FLAG_FP | rvisa::FLAG_VECTOR | rvisa::FLAG_MATRIX))
      return DynamicMicroOpBoomV1::UOP_PORT0;
   if (f & rvisa::FLAG_BUSY)
      return DynamicMicroOpBoomV1::UOP_PORT1;
   if (f & (rvisa::FLAG_LOAD | rvisa::FLAG_STORE))
      return DynamicMicroOpBoomV1::UOP_PORT2;
   return DynamicMicroOpBoomV1::UOP_PORT012;
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
   if (uop->uop_type != MicroOp::UOP_EXECUTE)
      return UOP_ALU_NONE;
   const uint32_t f = rvisa::flags_of(uop->getInstructionOpcode());
   if (f & rvisa::FLAG_MATRIX)
      return UOP_ALU_MATRIX;
   if (f & rvisa::FLAG_BUSY)
      return UOP_ALU_TRIG;          /* dividers and square roots: one busy unit */
   return UOP_ALU_NONE;
}

DynamicMicroOpBoomV1::DynamicMicroOpBoomV1(const MicroOp *uop, const CoreModel *core_model, ComponentPeriod period)
   : DynamicMicroOp(uop, core_model, period)
   , uop_port(DynamicMicroOpBoomV1::getPort(uop))
   , uop_alu(DynamicMicroOpBoomV1::getAlu(uop))
   , uop_bypass(DynamicMicroOpBoomV1::getBypassType(uop))
{
}
