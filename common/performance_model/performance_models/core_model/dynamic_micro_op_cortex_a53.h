#ifndef __DYNAMIC_MICRO_OP_CORTEX_A53_H
#define __DYNAMIC_MICRO_OP_CORTEX_A53_H

#include "dynamic_micro_op.h"

class MicroOp;

class DynamicMicroOpCortexA53 : public DynamicMicroOp
{
public:
    enum uop_port_t {
        UOP_PORT0,  // Branch
        UOP_PORT0_12,  // Branch and integer (2 microops)
        UOP_PORT1, // Integer 0 (MAC)
        UOP_PORT2, // Integer 1 (DIV)
        UOP_PORT12,  // Integer 0 or 1
        UOP_PORT3, // FP/ASIMD 0
        UOP_PORT4, // FP/ASIMD 1
        UOP_PORT34, // FP/ASIMD 0 or 1
        UOP_PORT5,  // Load and store
        UOP_PORT5_12, // Load/store and integer
        UOP_PORT_SIZE,
    };
    uop_port_t uop_port;

    enum uop_alu_t {
        UOP_ALU_NONE = 0,
        UOP_ALU_DIV,
        UOP_ALU_MUL,
        UOP_ALU_FPNEON0,
        UOP_ALU_FPNEON1,
        UOP_ALU_SIZE
    };
    uop_alu_t uop_alu;

    enum uop_bypass_t {
        UOP_BYPASS_NONE,
        UOP_BYPASS_SIZE
    };
    uop_bypass_t uop_bypass;

    enum uop_issue_slot_t {
        UOP_ISSUE_SLOT_00,
        UOP_ISSUE_SLOT_01,
        UOP_ISSUE_SLOT_10,
        UOP_ISSUE_SLOT_11
    };
    uop_issue_slot_t uop_issue_slot;

    static uop_issue_slot_t getIssueSlot(const MicroOp* uop);
    static uop_port_t getPort(const MicroOp *uop);
    static uop_bypass_t getBypassType(const MicroOp *uop);
    static uop_alu_t getAlu(const MicroOp *uop);

    virtual const char* getType() const;

    DynamicMicroOpCortexA53(const MicroOp *uop, const CoreModel *core_model, ComponentPeriod period);

    uop_issue_slot_t getIssueSlot() const { return uop_issue_slot; }
    uop_port_t getPort(void) const { return uop_port; }
    uop_bypass_t getBypassType(void) const { return uop_bypass; }
    uop_alu_t getAlu(void) const { return uop_alu; }

    static const char * PortTypeString(DynamicMicroOpCortexA53::uop_port_t port);
};

#endif // __DYNAMIC_MICRO_OP_CORTEX_A53_H
