#ifndef __DYNAMIC_MICRO_OP_CORTEX_A72_H
#define __DYNAMIC_MICRO_OP_CORTEX_A72_H

#include "dynamic_micro_op.h"

class MicroOp;

class DynamicMicroOpCortexA72 : public DynamicMicroOp
{
   public:
      enum uop_port_t {
         UOP_PORT0,  // Branch
         UOP_PORT0_12,  // Branch and integer (2 microops)
         UOP_PORT12,  // Integer 0 or 1
         UOP_PORT3,  // Integer Multi-cycle
         UOP_PORT12_3,  // Integer and Multi-cycle (2 microops)
         UOP_PORT4,  // FP/ASIMD 0
         UOP_PORT5,  // FP/ASIMD 1
         UOP_PORT6,  // Load
         UOP_PORT7,  // Store         
         UOP_PORT6_12,  // Load and integer
         UOP_PORT7_12,  // Store and integer
         UOP_PORT_SIZE,
      };
      uop_port_t uop_port;

      enum uop_alu_t {
         UOP_ALU_NONE = 0,
         UOP_ALU_MULDIV,
         UOP_ALU_SIZE,
      };
      uop_alu_t uop_alu;

      enum uop_bypass_t {
         UOP_BYPASS_NONE,
         UOP_BYPASS_SIZE
      };
      uop_bypass_t uop_bypass;

      static uop_port_t getPort(const MicroOp *uop);
      static uop_bypass_t getBypassType(const MicroOp *uop);
      static uop_alu_t getAlu(const MicroOp *uop);

      virtual const char* getType() const;

      DynamicMicroOpCortexA72(const MicroOp *uop, const CoreModel *core_model, ComponentPeriod period);

      uop_port_t getPort(void) const { return uop_port; }
      uop_bypass_t getBypassType(void) const { return uop_bypass; }
      uop_alu_t getAlu(void) const { return uop_alu; }

      static const char * PortTypeString(DynamicMicroOpCortexA72::uop_port_t port);
};

#endif // __DYNAMIC_MICRO_OP_CORTEX_A72_H
