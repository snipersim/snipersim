#ifndef __DYNAMIC_MICRO_OP_NEHALEM
#define __DYNAMIC_MICRO_OP_NEHALEM

#include "dynamic_micro_op.h"

class MicroOp;

class DynamicMicroOpNehalem : public DynamicMicroOp
{
   public:
      enum uop_port_t {
         UOP_PORT0,
         UOP_PORT1,
         UOP_PORT2,
         UOP_PORT34,
         UOP_PORT5,
         UOP_PORT05,
         UOP_PORT015,
         UOP_PORT_SIZE,
      };
      uop_port_t uop_port;

      enum uop_alu_t {
         UOP_ALU_NONE = 0,
         UOP_ALU_TRIG,
         UOP_ALU_SIZE,
      };
      uop_alu_t uop_alu;

      enum uop_bypass_t {
         UOP_BYPASS_NONE,
         UOP_BYPASS_LOAD_FP,
         UOP_BYPASS_FP_STORE,
         UOP_BYPASS_SIZE
      };
      uop_bypass_t uop_bypass;

      static uop_port_t getPort(const MicroOp *uop);
      static uop_bypass_t getBypassType(const MicroOp *uop);
      static uop_alu_t getAlu(const MicroOp *uop);

      virtual const char* getType() const;

      DynamicMicroOpNehalem(const MicroOp *uop, const CoreModel *core_model, ComponentPeriod period);

      uop_port_t getPort(void) const { return uop_port; }
      uop_bypass_t getBypassType(void) const { return uop_bypass; }
      uop_alu_t getAlu(void) const { return uop_alu; }

      static const char * PortTypeString(DynamicMicroOpNehalem::uop_port_t port);
};

#endif // __DYNAMIC_MICRO_OP_NEHALEM
