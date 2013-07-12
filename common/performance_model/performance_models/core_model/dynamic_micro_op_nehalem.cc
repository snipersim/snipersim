#include "dynamic_micro_op_nehalem.h"
#include "micro_op.h"

const char* DynamicMicroOpNehalem::getType() const
{
   return "nehalem";
}

const char* DynamicMicroOpNehalem::PortTypeString(DynamicMicroOpNehalem::uop_port_t port)
{
   switch(port)
   {
      case UOP_PORT0:   return "port0";
      case UOP_PORT1:   return "port1";
      case UOP_PORT2:   return "port2";
      case UOP_PORT34:  return "port34";
      case UOP_PORT5:   return "port5";
      case UOP_PORT05:  return "port05";
      case UOP_PORT015: return "port015";
      default:
         LOG_PRINT_ERROR("Unknown port type %d", port);
   }
}

DynamicMicroOpNehalem::uop_port_t DynamicMicroOpNehalem::getPort(const MicroOp *uop)
{
   switch(uop->uop_subtype) {
      case MicroOp::UOP_SUBTYPE_FP_ADDSUB:
         return DynamicMicroOpNehalem::UOP_PORT1;
      case MicroOp::UOP_SUBTYPE_FP_MULDIV:
         return DynamicMicroOpNehalem::UOP_PORT0;
      case MicroOp::UOP_SUBTYPE_LOAD:
         return DynamicMicroOpNehalem::UOP_PORT2;
      case MicroOp::UOP_SUBTYPE_STORE:
         return DynamicMicroOpNehalem::UOP_PORT34;
      case MicroOp::UOP_SUBTYPE_GENERIC:
         switch(uop->getInstructionOpcode()) {
            case XED_ICLASS_IMUL:
               return uop->getOperandSize() == 64 ? DynamicMicroOpNehalem::UOP_PORT0 : DynamicMicroOpNehalem::UOP_PORT1;
            case XED_ICLASS_LEA:
               return DynamicMicroOpNehalem::UOP_PORT1; // 0 on Core2, 1 on Nehalem, 0 (simple) or 1 (complex) on Sandy Bridge
            case XED_ICLASS_CVTPS2PD:
            case XED_ICLASS_CVTSS2SD:
            case XED_ICLASS_SQRTSS:
            case XED_ICLASS_SQRTPS:
            case XED_ICLASS_SQRTSD:
            case XED_ICLASS_SQRTPD:
            case XED_ICLASS_VSQRTSS:
            case XED_ICLASS_VSQRTPS:
            case XED_ICLASS_VSQRTSD:
            case XED_ICLASS_VSQRTPD:
               return DynamicMicroOpNehalem::UOP_PORT0;
            case XED_ICLASS_COMISD:
            case XED_ICLASS_COMISS:
            case XED_ICLASS_UCOMISD:
            case XED_ICLASS_UCOMISS:
            case XED_ICLASS_MAXSS:
            case XED_ICLASS_MAXSD:
            case XED_ICLASS_MAXPS:
            case XED_ICLASS_MAXPD:
            case XED_ICLASS_MINSS:
            case XED_ICLASS_MINSD:
            case XED_ICLASS_MINPS:
            case XED_ICLASS_MINPD:
            case XED_ICLASS_ROUNDPS:
            case XED_ICLASS_ROUNDPD:
            case XED_ICLASS_CVTPD2PS:
            case XED_ICLASS_CVTSD2SS:
            case XED_ICLASS_CVTDQ2PS:
            case XED_ICLASS_CVTPS2DQ:
            case XED_ICLASS_CVTTPS2DQ:
            case XED_ICLASS_CVTDQ2PD:
            case XED_ICLASS_CVTPD2DQ:
            case XED_ICLASS_CVTTPD2DQ:
            case XED_ICLASS_CVTPI2PS:
            case XED_ICLASS_CVTPS2PI:
            case XED_ICLASS_CVTTPS2PI:
            case XED_ICLASS_CVTPI2PD:
            case XED_ICLASS_CVTPD2PI:
            case XED_ICLASS_CVTTPD2PI:
            case XED_ICLASS_CVTSI2SS:
            case XED_ICLASS_CVTSS2SI:
            case XED_ICLASS_CVTTSS2SI:
            case XED_ICLASS_CVTSI2SD:
            case XED_ICLASS_CVTSD2SI:
            case XED_ICLASS_CVTTSD2SI:
            case XED_ICLASS_RSQRTSS:
            case XED_ICLASS_RSQRTPS:
            case XED_ICLASS_VRSQRTSS:
            case XED_ICLASS_VRSQRTPS:
               return DynamicMicroOpNehalem::UOP_PORT1;
            case XED_ICLASS_ANDPS:
            case XED_ICLASS_ANDPD:
            case XED_ICLASS_ANDNPS:
            case XED_ICLASS_ANDNPD:
            case XED_ICLASS_ORPS:
            case XED_ICLASS_ORPD:
            case XED_ICLASS_XORPS:
            case XED_ICLASS_XORPD:
            case XED_ICLASS_MOVAPD:
            case XED_ICLASS_MOVAPS:
            case XED_ICLASS_MOVHLPS:
            case XED_ICLASS_MOVHPD:
            case XED_ICLASS_MOVHPS:
            case XED_ICLASS_MOVLHPS:
            case XED_ICLASS_MOVLPD:
            case XED_ICLASS_MOVLPS:
            case XED_ICLASS_MOVMSKPD:
            case XED_ICLASS_MOVMSKPS:
            case XED_ICLASS_MOVSD_XMM:
            case XED_ICLASS_MOVSHDUP:
            case XED_ICLASS_MOVSLDUP:
            case XED_ICLASS_MOVSS:
            case XED_ICLASS_VMOVAPD:
            case XED_ICLASS_VMOVAPS:
            case XED_ICLASS_VMOVHLPS:
            case XED_ICLASS_VMOVHPD:
            case XED_ICLASS_VMOVHPS:
            case XED_ICLASS_VMOVLHPS:
            case XED_ICLASS_VMOVLPD:
            case XED_ICLASS_VMOVLPS:
            case XED_ICLASS_VMOVMSKPD:
            case XED_ICLASS_VMOVMSKPS:
            case XED_ICLASS_VMOVSD:
            case XED_ICLASS_VMOVSHDUP:
            case XED_ICLASS_VMOVSLDUP:
            case XED_ICLASS_VMOVSS:
            case XED_ICLASS_UNPCKHPD:
            case XED_ICLASS_UNPCKHPS:
            case XED_ICLASS_UNPCKLPD:
            case XED_ICLASS_UNPCKLPS:
               return DynamicMicroOpNehalem::UOP_PORT5;
            default:
               return DynamicMicroOpNehalem::UOP_PORT015;
         }
      case MicroOp::UOP_SUBTYPE_BRANCH:
         return DynamicMicroOpNehalem::UOP_PORT5;
      default:
         LOG_PRINT_ERROR("Unknown uop_subtype %u", uop->uop_subtype);
   }
}

DynamicMicroOpNehalem::uop_bypass_t DynamicMicroOpNehalem::getBypassType(const MicroOp *uop)
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

DynamicMicroOpNehalem::uop_alu_t DynamicMicroOpNehalem::getAlu(const MicroOp *uop)
{
   switch(uop->uop_type)
   {
      default:
         return UOP_ALU_NONE;
      case MicroOp::UOP_EXECUTE:
         switch(uop->getInstructionOpcode())
         {
            case XED_ICLASS_DIV:
            case XED_ICLASS_IDIV:
            case XED_ICLASS_FDIV:
            case XED_ICLASS_FIDIV:
            case XED_ICLASS_FDIVP:
            case XED_ICLASS_FDIVR:
            case XED_ICLASS_FDIVRP:
            case XED_ICLASS_DIVSS:
            case XED_ICLASS_DIVPS:
            case XED_ICLASS_DIVSD:
            case XED_ICLASS_DIVPD:
            case XED_ICLASS_VDIVSS:
            case XED_ICLASS_VDIVPS:
            case XED_ICLASS_VDIVSD:
            case XED_ICLASS_VDIVPD:
            case XED_ICLASS_SQRTSS:
            case XED_ICLASS_SQRTPS:
            case XED_ICLASS_SQRTSD:
            case XED_ICLASS_SQRTPD:
            case XED_ICLASS_VSQRTSS:
            case XED_ICLASS_VSQRTPS:
            case XED_ICLASS_VSQRTSD:
            case XED_ICLASS_VSQRTPD:
            case XED_ICLASS_RSQRTSS:
            case XED_ICLASS_RSQRTPS:
            case XED_ICLASS_VRSQRTSS:
            case XED_ICLASS_VRSQRTPS:
               return UOP_ALU_TRIG;
            default:
               return UOP_ALU_NONE;
         }
   }
}

DynamicMicroOpNehalem::DynamicMicroOpNehalem(const MicroOp *uop, const CoreModel *core_model, ComponentPeriod period)
   : DynamicMicroOp(uop, core_model, period)
   , uop_port(DynamicMicroOpNehalem::getPort(uop))
   , uop_alu(DynamicMicroOpNehalem::getAlu(uop))
   , uop_bypass(DynamicMicroOpNehalem::getBypassType(uop))
{
}
