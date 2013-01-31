#include "core.h"
#include "iocoom_performance_model.h"

#include "log.h"
#include "dynamic_instruction_info.h"
#include "config.hpp"
#include "simulator.h"
#include "branch_predictor.h"
#include "dvfs_manager.h"

IOCOOMPerformanceModel::IOCOOMPerformanceModel(Core *core)
   : PerformanceModel(core)
   , m_register_scoreboard(512, SubsecondTime())
   , m_store_buffer(0)
   , m_load_unit(0)
{
   config::Config *cfg = Sim()->getCfg();

   try
   {
      m_store_buffer = new StoreBuffer(cfg->getIntArray("perf_model/core/iocoom/num_store_buffer_entries", core->getId()));
      m_load_unit = new LoadUnit(cfg->getIntArray("perf_model/core/iocoom/num_outstanding_loads", core->getId()));
   }
   catch (...)
   {
      LOG_PRINT_ERROR("Config info not available.");
   }
}

IOCOOMPerformanceModel::~IOCOOMPerformanceModel()
{
   delete m_load_unit;
   delete m_store_buffer;
}

bool IOCOOMPerformanceModel::handleInstruction(Instruction const* instruction)
{
   ComponentTime cost = m_elapsed_time.getLatencyGenerator();

   SubsecondTime i_cost = instruction->getCost(getCore());
   if (i_cost == PerformanceModel::DyninsninfoNotAvailable())
      return false;
   cost.addLatency(instruction->getCost(getCore()));


   // icache modeling
   if (instruction->getAddress())
      modelIcache(instruction->getAddress(), instruction->getSize());

   /*
      model instruction in the following steps:
      - find when read operations are available
      - find latency of instruction
      - update write operands
   */
   const OperandList &ops = instruction->getOperands();

   // buffer write operands to be updated after instruction executes
   DynamicInstructionInfoQueue write_info;

   // find when read operands are available
   SubsecondTime read_operands_ready = m_elapsed_time.getElapsedTime();
   SubsecondTime write_operands_ready = m_elapsed_time.getElapsedTime();
   SubsecondTime max_load_latency = SubsecondTime::Zero();

   // REG read operands
   for (unsigned int i = 0; i < ops.size(); i++)
   {
      const Operand &o = ops[i];

      if (o.m_direction != Operand::READ)
         continue;

      if (o.m_type != Operand::REG)
         continue;

      LOG_ASSERT_ERROR(o.m_value < m_register_scoreboard.size(),
                       "Register value out of range: %llu", o.m_value);

      if (m_register_scoreboard[o.m_value] > read_operands_ready)
         read_operands_ready = m_register_scoreboard[o.m_value];
   }

   // MEMORY read & write operands
   for (unsigned int i = 0; i < ops.size(); i++)
   {
      const Operand &o = ops[i];

      if (o.m_type != Operand::MEMORY)
         continue;

      DynamicInstructionInfo *info = getDynamicInstructionInfo(*instruction);
      if (!info)
         return false;

      if (o.m_direction == Operand::READ)
      {
         LOG_ASSERT_ERROR(info->type == DynamicInstructionInfo::MEMORY_READ,
                          "Expected memory read info, got: %d.", info->type);

         std::pair<SubsecondTime,SubsecondTime> load_timing_info = executeLoad(read_operands_ready, *info);
         SubsecondTime load_ready = load_timing_info.first;
         SubsecondTime load_latency = load_timing_info.second;

         if (max_load_latency < load_latency)
            max_load_latency = load_latency;

         // This 'ready' is related to a structural hazard in the LOAD Unit
         if (read_operands_ready < load_ready)
            read_operands_ready = load_ready;
      }
      else
      {
         LOG_ASSERT_ERROR(info->type == DynamicInstructionInfo::MEMORY_WRITE,
                          "Expected memory write info, got: %d.", info->type);

         write_info.push(*info);
      }

      popDynamicInstructionInfo();
   }

   // update cycle count with instruction cost
   m_instruction_count++;
   // This is the completion time of an instruction
   // leaving out the register and memory write
   SubsecondTime execute_unit_completion_time = read_operands_ready + max_load_latency + cost.getElapsedTime();

   if (m_elapsed_time.getElapsedTime() < execute_unit_completion_time)
      m_elapsed_time.getElapsedTime() = execute_unit_completion_time;

   // REG write operands
   // In this core model, we directly resolve WAR hazards since we wait
   // for all the read operands of an instruction to be available before
   // we issue it
   // Assume that the register file can be written in one cycle
   for (unsigned int i = 0; i < ops.size(); i++)
   {
      const Operand &o = ops[i];

      if (o.m_direction != Operand::WRITE)
         continue;

      if (o.m_type != Operand::REG)
         continue;

      // Note that m_elapsed_time can be less then the previous value
      // of m_register_scoreboard[o.m_value]
      m_register_scoreboard[o.m_value] = execute_unit_completion_time;
      if (write_operands_ready < m_register_scoreboard[o.m_value])
         write_operands_ready = m_register_scoreboard[o.m_value];
   }

   // MEMORY write operands
   // This is done before doing register
   // operands to make sure the scoreboard is updated correctly
   for (unsigned int i = 0; i < ops.size(); i++)
   {
      const Operand &o = ops[i];

      if (o.m_direction != Operand::WRITE)
         continue;

      if (o.m_type != Operand::MEMORY)
         continue;

      const DynamicInstructionInfo &info = write_info.front();
      // This just updates the contents of the store buffer
      SubsecondTime store_time = executeStore(execute_unit_completion_time, info);
      write_info.pop();

      if (write_operands_ready < store_time)
         write_operands_ready = store_time;
   }

   if (m_elapsed_time.getElapsedTime() < write_operands_ready)
      m_elapsed_time.setElapsedTime(write_operands_ready);

   LOG_ASSERT_ERROR(write_info.empty(), "Some write info left over?");

   return true;
}

std::pair<const SubsecondTime,const SubsecondTime>
IOCOOMPerformanceModel::executeLoad(SubsecondTime time, const DynamicInstructionInfo &info)
{
   bool l1_hit = info.memory_info.hit_where == HitWhere::L1_OWN;

   // similarly, a miss in the l1 with a completed entry in the store
   // buffer is treated as an invalidation
   StoreBuffer::Status status = m_store_buffer->isAddressAvailable(time, info.memory_info.addr);

   if ((status == StoreBuffer::VALID) || (l1_hit && status == StoreBuffer::COMPLETED))
      return std::pair<const SubsecondTime,const SubsecondTime>(time,SubsecondTime::Zero());

   // a miss in the l1 forces a miss in the store buffer
   SubsecondTime latency = info.memory_info.latency;

   return std::pair<const SubsecondTime,const SubsecondTime>(m_load_unit->execute(time, latency), latency);
}

SubsecondTime IOCOOMPerformanceModel::executeStore(SubsecondTime time, const DynamicInstructionInfo &info)
{
   SubsecondTime latency = info.memory_info.latency;

   return m_store_buffer->executeStore(time, latency, info.memory_info.addr);
}

void IOCOOMPerformanceModel::modelIcache(IntPtr addr, UInt32 size)
{
   SubsecondTime access_time = getCore()->readInstructionMemory(addr, size).latency;
   m_elapsed_time.addLatency(access_time);
}

// Helper classes

IOCOOMPerformanceModel::LoadUnit::LoadUnit(unsigned int num_units)
   : m_scoreboard(num_units, SubsecondTime())
{}

IOCOOMPerformanceModel::LoadUnit::~LoadUnit()
{
}

SubsecondTime IOCOOMPerformanceModel::LoadUnit::execute(SubsecondTime time, SubsecondTime occupancy)
{
   UInt64 unit = 0;

   for (unsigned int i = 0; i < m_scoreboard.size(); i++)
   {
      if (m_scoreboard[i] <= time)
      {
         // a unit is available
         m_scoreboard[i] = time + occupancy;
         return time;
      }
      else
      {
         if (m_scoreboard[i] < m_scoreboard[unit])
            unit = i;
      }
   }

   // update unit, return time available
   SubsecondTime time_avail = m_scoreboard[unit];
   m_scoreboard[unit] += occupancy;
   return time_avail;
}

IOCOOMPerformanceModel::StoreBuffer::StoreBuffer(unsigned int num_entries)
   : m_scoreboard(num_entries, SubsecondTime())
   , m_addresses(num_entries, static_cast<IntPtr>(0))
{}

IOCOOMPerformanceModel::StoreBuffer::~StoreBuffer()
{
}

SubsecondTime IOCOOMPerformanceModel::StoreBuffer::executeStore(SubsecondTime time, SubsecondTime occupancy, IntPtr addr)
{
   // Note: basically identical to ExecutionUnit, except we need to
   // track addresses as well

   // is address already in buffer?
   for (unsigned int i = 0; i < m_scoreboard.size(); i++)
   {
      if (m_addresses[i] == addr)
      {
         m_scoreboard[i] = time + occupancy;
         return time;
      }
   }

   // if not, find earliest available entry
   unsigned int unit = 0;

   for (unsigned int i = 0; i < m_scoreboard.size(); i++)
   {
      if (m_scoreboard[i] <= time)
      {
         // a unit is available
         m_scoreboard[i] = time + occupancy;
         m_addresses[i] = addr;
         return time;
      }
      else
      {
         if (m_scoreboard[i] < m_scoreboard[unit])
            unit = i;
      }
   }

   // update unit, return time available
   SubsecondTime time_avail = m_scoreboard[unit];
   m_scoreboard[unit] += occupancy;
   m_addresses[unit] = addr;
   return time_avail;
}

IOCOOMPerformanceModel::StoreBuffer::Status IOCOOMPerformanceModel::StoreBuffer::isAddressAvailable(SubsecondTime time, IntPtr addr)
{
   for (unsigned int i = 0; i < m_scoreboard.size(); i++)
   {
      if (m_addresses[i] == addr)
      {
         if (m_scoreboard[i] >= time)
            return VALID;
         else
            return COMPLETED;
      }
   }

   return NOT_FOUND;
}
