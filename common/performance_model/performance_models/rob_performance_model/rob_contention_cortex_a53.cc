/*
 * This file is covered under the Interval Academic License, see LICENCE.academic
 */

#include "rob_contention_cortex_a53.h"
#include "core_model.h"
#include "dynamic_micro_op.h"
#include "core.h"
#include "config.hpp"
#include "simulator.h"
#include "memory_manager_base.h"

RobContentionCortexA53::RobContentionCortexA53(const Core *core, const CoreModel *core_model)
    : m_core_model(core_model)
    , m_cache_block_mask(~(core->getMemoryManager()->getCacheBlockSize() - 1))
    , m_now(core->getDvfsDomain())
    , alu_used_until(DynamicMicroOpCortexA53::UOP_ALU_SIZE, SubsecondTime::Zero())
{
}

void RobContentionCortexA53::initCycle(SubsecondTime now)
{
    m_now.setElapsedTime(now);
    port_branch = false;
    port_simd0 = false;
    port_simd1 = false;
    port_integer0 = false;
    port_integer1 = false;
    port_ldst = false;
    slot0 = false;
    slot1 = false;
}

inline DynamicMicroOpCortexA53::uop_alu_t RobContentionCortexA53::issueIntegerPorts() {
    if (port_integer0) {
        port_integer1 = true;
        return DynamicMicroOpCortexA53::uop_alu_t::UOP_ALU_DIV;
    }
    else if (port_integer1) {
        port_integer0 = true;
        return DynamicMicroOpCortexA53::uop_alu_t::UOP_ALU_MUL;
    }
    else {
        // TODO: Since both integer ports are free, a decision must be made, and
        // it can affect incoming micro ops. For the moment I will always choose
        // the first one (the integer multiplication pipe).
        // Possible fix: Check if the port_integer0 ALU can be used.
        port_integer0 = true;
        return DynamicMicroOpCortexA53::uop_alu_t::UOP_ALU_MUL;
    }
}

inline DynamicMicroOpCortexA53::uop_alu_t RobContentionCortexA53::issueNEONPorts() {
    if (port_simd0) {
        port_simd1 = true;
        return DynamicMicroOpCortexA53::uop_alu_t::UOP_ALU_FPNEON1;
    }
    else if (port_simd1) {
        port_simd0 = true;
        return DynamicMicroOpCortexA53::uop_alu_t::UOP_ALU_FPNEON0;
    }
    else {
        //TODO: Same as in issueIntegerPorts
        port_simd0 = true;
        return DynamicMicroOpCortexA53::uop_alu_t::UOP_ALU_FPNEON0;
    }
}

bool RobContentionCortexA53::tryIssue(const DynamicMicroOp &uop)
{
    // Port contention
    // TODO: Maybe the scheduler is more intelligent and doesn't just assing the first uop in the ROB
    //       that fits a particular free port. We could give precedence to uops that have dependants, etc.
    // NOTE: mixes canIssue and doIssue, in the sense that ports* are incremented immediately.
    //       This works as long as, if we return true, this microop is indeed issued

    const DynamicMicroOpCortexA53 *core_uop_info = uop.getCoreSpecificInfo<DynamicMicroOpCortexA53>();
    DynamicMicroOpCortexA53::uop_port_t uop_port = core_uop_info->getPort();
    DynamicMicroOpCortexA53::uop_issue_slot_t uop_issue_slot = core_uop_info->getIssueSlot();
    DynamicMicroOpCortexA53::uop_alu_t alu = DynamicMicroOpCortexA53::uop_alu_t::UOP_ALU_NONE;

    bool canIssue = true;
    bool prevSlot0 = slot0;
    bool prevSlot1 = slot1;

    switch (uop_issue_slot) {
    case DynamicMicroOpCortexA53::UOP_ISSUE_SLOT_00:
        if (slot0 || slot1) {
            return false;
        }
        // This is to avoid other instructions that can be dual issued to be issued
        // after this instruction has been already issued.
        slot0 = slot1 = true;
        break;
    case DynamicMicroOpCortexA53::UOP_ISSUE_SLOT_01:
        if (slot0) {
            return false;
        }
        slot0 = true;
        break;
    case DynamicMicroOpCortexA53::UOP_ISSUE_SLOT_10:
        if (slot1) {
            return false;
        }
        slot1 = true;
        break;
    case DynamicMicroOpCortexA53::UOP_ISSUE_SLOT_11:
        if (slot0 && slot1) {
            return false;
        }
        if (slot0) {
            slot1 = true;
        }
        else {
            slot0 = true;
        }
        break;
    }

    if (uop_port == DynamicMicroOpCortexA53::UOP_PORT0)
    {
        if (port_branch)
            canIssue = false;
        else
            port_branch = true;
    }
    else if (uop_port == DynamicMicroOpCortexA53::UOP_PORT0_12)
    {
        if (port_branch || (port_integer0 && port_integer1))
            canIssue = false;
        else
        {
            port_branch = true;
            alu = issueIntegerPorts();
        }
    }
    else if (uop_port == DynamicMicroOpCortexA53::UOP_PORT12)
    {
        if (port_integer0 && port_integer1)
            canIssue = false;
        else {
            alu = issueIntegerPorts();
        }
    }
    else if (uop_port == DynamicMicroOpCortexA53::UOP_PORT3) {
        if (port_simd0) {
            canIssue = false;
        }
        else {
            alu = DynamicMicroOpCortexA53::uop_alu_t::UOP_ALU_FPNEON0;
            port_simd0 = true;
        }
    }
    else if (uop_port == DynamicMicroOpCortexA53::UOP_PORT4) {
        if (port_simd1) {
            canIssue = false;
        }
        else {
            alu = DynamicMicroOpCortexA53::uop_alu_t::UOP_ALU_FPNEON1;
            port_simd1 = true;
        }
    }
    else if (uop_port == DynamicMicroOpCortexA53::UOP_PORT34) {
        if (port_simd0 && port_simd1)
            canIssue = false;
        else
            alu = issueNEONPorts();
    }
    else if (uop_port == DynamicMicroOpCortexA53::UOP_PORT5)
    {
        if (port_ldst)
            canIssue = false;
        else
            port_ldst = true;
    }
    else if (uop_port == DynamicMicroOpCortexA53::UOP_PORT1)
    {
        if (port_integer0)
            canIssue = false;
        else {
            port_integer0 = true;
            alu = DynamicMicroOpCortexA53::uop_alu_t::UOP_ALU_MUL;
        }
    }
    else if (uop_port == DynamicMicroOpCortexA53::UOP_PORT2)
    {
        if (port_integer1)
            canIssue = false;
        else {
            port_integer1 = true;
            alu = DynamicMicroOpCortexA53::uop_alu_t::UOP_ALU_DIV;
        }
    }
    else if (uop_port == DynamicMicroOpCortexA53::UOP_PORT5_12) {
        if (port_ldst || (port_integer0 && port_integer1)) {
            canIssue = false;
        }
        else {
            port_ldst = true;
            alu = issueIntegerPorts();
        }
    }

    if (!canIssue) {
        slot0 = prevSlot0;
        slot1 = prevSlot1;
        return false;
    }

    // ALU contention
    if (alu) {
        if (alu_used_until[alu] > m_now)
            return false;
        else {
            unsigned int aluLatency = m_core_model->getAluLatency(uop.getMicroOp());
            if (aluLatency > 0)
                alu_used_until[alu] = m_now + aluLatency;
        }
    }
    return true;
}

void RobContentionCortexA53::doIssue(DynamicMicroOp &uop)
{
//    const DynamicMicroOpCortexA53 *core_uop_info = uop.getCoreSpecificInfo<DynamicMicroOpCortexA53>();
//    DynamicMicroOpCortexA53::uop_alu_t alu = core_uop_info->getAlu();
//    if (alu) {
//        unsigned int aluLatency = m_core_model->getAluLatency(uop.getMicroOp());
//        if (aluLatency > 0)
//            alu_used_until[alu] = m_now + aluLatency;
//    }
}

bool RobContentionCortexA53::noMore()
{
    // With an issue width of 3 instructions and 6 ports, the ports won't be clogged
    return false;
}
