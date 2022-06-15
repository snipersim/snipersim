/*
 * This file is covered under the Interval Academic License, see LICENCE.academic
 */

#include "rob_contention_cortex_a72.h"
#include "core_model.h"
#include "dynamic_micro_op.h"
#include "core.h"
#include "config.hpp"
#include "simulator.h"
#include "memory_manager_base.h"

RobContentionCortexA72::RobContentionCortexA72(const Core *core, const CoreModel *core_model)
   : m_core_model(core_model)
   , m_cache_block_mask(~(core->getMemoryManager()->getCacheBlockSize() - 1))
   , m_now(core->getDvfsDomain())
   , alu_used_until(DynamicMicroOpCortexA72::UOP_ALU_SIZE, SubsecondTime::Zero())
{
}

void RobContentionCortexA72::initCycle(SubsecondTime now)
{
   m_now.setElapsedTime(now);
   port_branch = false;
   port_simd0 = false;
   port_simd1 = false;
   port_int_multi = false;
   port_ld = false; 
   port_st = false; 
   ports_integer = 0;
}

bool RobContentionCortexA72::tryIssue(const DynamicMicroOp &uop)
{
   // Port contention
   // TODO: Maybe the scheduler is more intelligent and doesn't just assing the first uop in the ROB
   //       that fits a particular free port. We could give precedence to uops that have dependants, etc.
   // NOTE: mixes canIssue and doIssue, in the sense that ports* are incremented immediately.
   //       This works as long as, if we return true, this microop is indeed issued

   const DynamicMicroOpCortexA72 *core_uop_info = uop.getCoreSpecificInfo<DynamicMicroOpCortexA72>();
   DynamicMicroOpCortexA72::uop_port_t uop_port = core_uop_info->getPort();
   
   if (uop_port == DynamicMicroOpCortexA72::UOP_PORT0)
   {
      if (port_branch)
         return false;
      else
         port_branch = true;
   }
   else if (uop_port == DynamicMicroOpCortexA72::UOP_PORT0_12)
   {
      if (port_branch || ports_integer >= 2)
         return false;
      else
      {
         port_branch = true;
         ports_integer++;
      }
   }
   else if (uop_port == DynamicMicroOpCortexA72::UOP_PORT12)
   {
      if (ports_integer >= 2)
         return false;
      else
         ports_integer++;
   }
   else if (uop_port == DynamicMicroOpCortexA72::UOP_PORT3)
   {
      if (port_int_multi)
         return false;
      else
         port_int_multi = true;
   }
   else if (uop_port == DynamicMicroOpCortexA72::UOP_PORT12_3)
   {
      if (port_int_multi || ports_integer >= 2)
         return false;
      else
      {
         port_int_multi = true;
         ports_integer++;
      }
   }
   else if (uop_port == DynamicMicroOpCortexA72::UOP_PORT4)
   {
      if (port_simd0 && port_simd1)
         return false;
      else if (port_simd0)
         port_simd1 = true;
      else
         port_simd0 = true;
   }
   else if (uop_port == DynamicMicroOpCortexA72::UOP_PORT5)
   {
      if (port_simd1 && port_simd0)
         return false;
      else if (port_simd1)
         port_simd0 = true;      
      else
         port_simd1 = true;
   }
   else if (uop_port == DynamicMicroOpCortexA72::UOP_PORT6)
   {
      if (port_ld)
         return false;
      else
         port_ld = true;
   }
   else if (uop_port == DynamicMicroOpCortexA72::UOP_PORT6_12)
   {
      if (port_ld || ports_integer >= 2)
         return false;
      else
      {
         port_ld = true;
         ports_integer++;
      }
   }     
   else if (uop_port == DynamicMicroOpCortexA72::UOP_PORT7)
   {
      if (port_st)
         return false;
      else
         port_st = true;
   }   
   else if (uop_port == DynamicMicroOpCortexA72::UOP_PORT7_12)
   {
      if (port_st || ports_integer >= 2)
         return false;
      else
      {
         port_st = true;
         ports_integer++;
      }
   } 

   // ALU contention
   if (DynamicMicroOpCortexA72::uop_alu_t alu = core_uop_info->getAlu())
   {
      if (alu_used_until[alu] > m_now)
         return false;
   }

   return true;
}

void RobContentionCortexA72::doIssue(DynamicMicroOp &uop)
{
   const DynamicMicroOpCortexA72 *core_uop_info = uop.getCoreSpecificInfo<DynamicMicroOpCortexA72>();
   DynamicMicroOpCortexA72::uop_alu_t alu = core_uop_info->getAlu();
   if (alu)
      alu_used_until[alu] = m_now + m_core_model->getAluLatency(uop.getMicroOp());
}

bool RobContentionCortexA72::noMore()
{
   // With an issue width of 3 instructions and 6 ports, the ports won't be clogged
   return false;
}
