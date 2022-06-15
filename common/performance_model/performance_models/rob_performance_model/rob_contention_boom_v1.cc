/*
 * This file is covered under the Interval Academic License, see LICENCE.academic
 */

#include "rob_contention_boom_v1.h"
#include "core_model.h"
#include "dynamic_micro_op.h"
#include "core.h"
#include "config.hpp"
#include "simulator.h"
#include "memory_manager_base.h"

RobContentionBoomV1::RobContentionBoomV1(const Core *core, const CoreModel *core_model)
   : m_core_model(core_model)
   , m_cache_block_mask(~(core->getMemoryManager()->getCacheBlockSize() - 1))
   , m_now(core->getDvfsDomain())
   , alu_used_until(DynamicMicroOpBoomV1::UOP_ALU_SIZE, SubsecondTime::Zero())
{
}

void RobContentionBoomV1::initCycle(SubsecondTime now)
{
   m_now.setElapsedTime(now);
   memset(ports, 0, sizeof(bool) * DynamicMicroOpBoomV1::UOP_PORT_SIZE);
   ports_generic012 = 0;
}

bool RobContentionBoomV1::tryIssue(const DynamicMicroOp &uop)
{
   // Port contention
   // TODO: Maybe the scheduler is more intelligent and doesn't just assing the first uop in the ROB
   //       that fits a particular free port. We could give precedence to uops that have dependants, etc.
   // NOTE: mixes canIssue and doIssue, in the sense that ports* are incremented immediately.
   //       This works as long as, if we return true, this microop is indeed issued

   const DynamicMicroOpBoomV1 *core_uop_info = uop.getCoreSpecificInfo<DynamicMicroOpBoomV1>();
   DynamicMicroOpBoomV1::uop_port_t uop_port = core_uop_info->getPort();

   if (uop_port == DynamicMicroOpBoomV1::UOP_PORT012)
   {
      if (ports_generic012 >= 3)
         return false;
      else
         ports_generic012++;
   }
   else
   { // PORT0, PORT1 or PORT2
      if (ports[uop_port])
         return false;
      else if (ports_generic012 >= 3)
         return false;
      else
      {
         ports[uop_port] = true;
         ports_generic012++;
      }
   }

   // ALU contention
   if (DynamicMicroOpBoomV1::uop_alu_t alu = core_uop_info->getAlu())
   {
      if (alu_used_until[alu] > m_now)
         return false;
   }

   return true;
}

void RobContentionBoomV1::doIssue(DynamicMicroOp &uop)
{
   const DynamicMicroOpBoomV1 *core_uop_info = uop.getCoreSpecificInfo<DynamicMicroOpBoomV1>();
   DynamicMicroOpBoomV1::uop_alu_t alu = core_uop_info->getAlu();
   if (alu)
      alu_used_until[alu] = m_now + m_core_model->getAluLatency(uop.getMicroOp());
}

bool RobContentionBoomV1::noMore()
{
   // When we issued something to all ports in this cycle, stop walking the rest of the ROB
   if (ports[DynamicMicroOpBoomV1::UOP_PORT2] && ports_generic012 >= 3)
      return true;
   else
      return false;
}
