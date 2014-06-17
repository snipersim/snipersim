/*
 * This file is covered under the Interval Academic License, see LICENCE.interval
 */

#include "rob_contention_nehalem.h"
#include "core_model.h"
#include "dynamic_micro_op.h"
#include "core.h"
#include "config.hpp"
#include "simulator.h"
#include "memory_manager_base.h"

RobContentionNehalem::RobContentionNehalem(const Core *core, const CoreModel *core_model)
   : m_core_model(core_model)
   , m_cache_block_mask(~(core->getMemoryManager()->getCacheBlockSize() - 1))
   , m_now(core->getDvfsDomain())
   , alu_used_until(DynamicMicroOpNehalem::UOP_ALU_SIZE, SubsecondTime::Zero())
{
}

void RobContentionNehalem::initCycle(SubsecondTime now)
{
   m_now.setElapsedTime(now);
   memset(ports, 0, sizeof(bool) * DynamicMicroOpNehalem::UOP_PORT_SIZE);
   ports_generic = 0;
   ports_generic05 = 0;
}

bool RobContentionNehalem::tryIssue(const DynamicMicroOp &uop)
{
   // Port contention
   // TODO: Maybe the scheduler is more intelligent and doesn't just assing the first uop in the ROB
   //       that fits a particular free port. We could give precedence to uops that have dependants, etc.
   // NOTE: mixes canIssue and doIssue, in the sense that ports* are incremented immediately.
   //       This works as long as, if we return true, this microop is indeed issued

   const DynamicMicroOpNehalem *core_uop_info = uop.getCoreSpecificInfo<DynamicMicroOpNehalem>();
   DynamicMicroOpNehalem::uop_port_t uop_port = core_uop_info->getPort();
   if (uop_port == DynamicMicroOpNehalem::UOP_PORT015)
   {
      if (ports_generic >= 3)
         return false;
      else
         ports_generic++;
   }
   else if (uop_port == DynamicMicroOpNehalem::UOP_PORT05)
   {
      if (ports_generic05 >= 2)
         return false;
      else
         ports_generic05++;
   }
   else if (uop_port == DynamicMicroOpNehalem::UOP_PORT2 || uop_port == DynamicMicroOpNehalem::UOP_PORT34)
   {
      if (ports[uop_port])
         return false;
      else
         ports[uop_port] = true;
   }
   else
   { // PORT0, PORT1 or PORT5
      if (ports[uop_port])
         return false;
      else if (ports_generic >= 3)
         return false;
      else if (uop_port != DynamicMicroOpNehalem::UOP_PORT1 && ports_generic05 >= 2)
         return false;
      else
      {
         ports[uop_port] = true;
         ports_generic++;
         if (uop_port != DynamicMicroOpNehalem::UOP_PORT1)
            ports_generic05++;
      }
   }

   // ALU contention
   if (DynamicMicroOpNehalem::uop_alu_t alu = core_uop_info->getAlu())
   {
      if (alu_used_until[alu] > m_now)
         return false;
   }

   return true;
}

void RobContentionNehalem::doIssue(DynamicMicroOp &uop)
{
   const DynamicMicroOpNehalem *core_uop_info = uop.getCoreSpecificInfo<DynamicMicroOpNehalem>();
   DynamicMicroOpNehalem::uop_alu_t alu = core_uop_info->getAlu();
   if (alu)
      alu_used_until[alu] = m_now + m_core_model->getAluLatency(uop.getMicroOp());
}

bool RobContentionNehalem::noMore()
{
   // When we issued something to all ports in this cycle, stop walking the rest of the ROB
   if (ports[DynamicMicroOpNehalem::UOP_PORT2] && ports[DynamicMicroOpNehalem::UOP_PORT34] && ports_generic >= 3)
      return true;
   else
      return false;
}
