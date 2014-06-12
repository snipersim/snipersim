#include "instruction.h"
#include "simulator.h"
#include "core_manager.h"
#include "core.h"
#include "performance_model.h"
#include "branch_predictor.h"
#include "config.hpp"

// Instruction

Instruction::StaticInstructionCosts Instruction::m_instruction_costs;

Instruction::Instruction(InstructionType type, OperandList &operands)
   : m_type(type)
   , m_uops(NULL)
   , m_addr(0)
   , m_operands(operands)
{
}

Instruction::Instruction(InstructionType type)
   : m_type(type)
   , m_uops(NULL)
   , m_addr(0)
{
}

InstructionType Instruction::getType() const
{
    return m_type;
}

String Instruction::getTypeName() const
{
   LOG_ASSERT_ERROR(m_type < MAX_INSTRUCTION_COUNT, "Unknown instruction type: %d", m_type);
   return String( INSTRUCTION_NAMES[ getType() ] );
}

// An instruction cost is the number of cycles it takes to execute the instruction, times the
// period of the processor that we are currently running on
SubsecondTime Instruction::getCost(Core *core) const
{
   LOG_ASSERT_ERROR(m_type < MAX_INSTRUCTION_COUNT, "Unknown instruction type: %d", m_type);
   const ComponentPeriod *period = core->getDvfsDomain();
   return static_cast<SubsecondTime>(*period) * Instruction::m_instruction_costs[m_type];
}

void Instruction::initializeStaticInstructionModel()
{
   m_instruction_costs.resize(MAX_INSTRUCTION_COUNT);
   for(unsigned int i = 0; i < MAX_INSTRUCTION_COUNT; i++)
   {
       char key_name [1024];
       snprintf(key_name, 1024, "perf_model/core/static_instruction_costs/%s", INSTRUCTION_NAMES[i]);
       UInt32 instruction_cost = Sim()->getCfg()->getInt(key_name);
       m_instruction_costs[i] = instruction_cost;
   }
}

// PseudoInstruction

PseudoInstruction::PseudoInstruction(SubsecondTime cost, InstructionType type)
   : Instruction(type)
   , m_cost(cost)
{
}

PseudoInstruction::~PseudoInstruction()
{
}

SubsecondTime PseudoInstruction::getCost(Core *core) const
{
   return m_cost;
}

// SyncInstruction

SyncInstruction::SyncInstruction(SubsecondTime time, sync_type_t sync_type)
   : PseudoInstruction(SubsecondTime::Zero(), INST_SYNC)
   , m_time(time)
   , m_sync_type(sync_type)
{ }

SubsecondTime SyncInstruction::getCost(Core *core) const
{
   LOG_ASSERT_ERROR(false, "SyncInstruction::getCost() called, this instruction should not have made it into handleInstruction");
   return SubsecondTime::Zero();
}


// SpawnInstruction

SpawnInstruction::SpawnInstruction(SubsecondTime time)
   : PseudoInstruction(SubsecondTime::Zero(), INST_SPAWN)
   , m_time(time)
{ }

SubsecondTime SpawnInstruction::getCost(Core *core) const
{
   LOG_ASSERT_ERROR(false, "SpawnInstruction::getCost() called, this instruction should not have made it into handleInstruction");
   return SubsecondTime::Zero();
}

SubsecondTime SpawnInstruction::getTime() const
{
   return m_time;
}

// BranchInstruction

BranchInstruction::BranchInstruction(OperandList &l)
   : Instruction(INST_BRANCH, l)
{ }
