/*
 * This file is covered under the Interval Academic License, see LICENCE.academic
 */

#include "interval_contention_boom_v1.h"
#include "core.h"
#include "stats.h"

IntervalContentionBoomV1::IntervalContentionBoomV1(const Core *core, const CoreModel *core_model)
   : m_core_model(core_model)
{
   for(unsigned int i = 0; i < DynamicMicroOpBoomV1::UOP_PORT_SIZE; ++i)
   {
      m_cpContrByPort[i] = 0;
      String name = String("cpContr_") + DynamicMicroOpBoomV1::PortTypeString((DynamicMicroOpBoomV1::uop_port_t)i);
      registerStatsMetric("interval_timer", core->getId(), name, &(m_cpContrByPort[i]));
   }
}

void IntervalContentionBoomV1::clearFunctionalUnitStats()
{
   for(unsigned int i = 0; i < (unsigned int)DynamicMicroOpBoomV1::UOP_PORT_SIZE; ++i)
   {
      m_count_byport[i] = 0;
   }
}

void IntervalContentionBoomV1::addFunctionalUnitStats(const DynamicMicroOp *uop)
{
   m_count_byport[uop->getCoreSpecificInfo<DynamicMicroOpBoomV1>()->getPort()]++;
}

void IntervalContentionBoomV1::removeFunctionalUnitStats(const DynamicMicroOp *uop)
{
   m_count_byport[uop->getCoreSpecificInfo<DynamicMicroOpBoomV1>()->getPort()]--;
}

uint64_t IntervalContentionBoomV1::getEffectiveCriticalPathLength(uint64_t critical_path_length, bool update_reason)
{
   DynamicMicroOpBoomV1::uop_port_t reason = DynamicMicroOpBoomV1::UOP_PORT_SIZE;
   uint64_t effective_cp_length = critical_path_length;

   // For the standard ports, check if we have exceeded our execution limit
   for (unsigned int i = 0 ; i < DynamicMicroOpBoomV1::UOP_PORT_SIZE ; i++)
   {
      // Skip shared ports
      if (i != DynamicMicroOpBoomV1::UOP_PORT012 && effective_cp_length < m_count_byport[i]
      )
      {
         effective_cp_length = m_count_byport[i];
         reason = (DynamicMicroOpBoomV1::uop_port_t)i;
      }
   }
   // Check shared port usage
   uint64_t port012_use = m_count_byport[DynamicMicroOpBoomV1::UOP_PORT0] + m_count_byport[DynamicMicroOpBoomV1::UOP_PORT1]
                        + m_count_byport[DynamicMicroOpBoomV1::UOP_PORT2] + m_count_byport[DynamicMicroOpBoomV1::UOP_PORT012];
   if (port012_use > (3*effective_cp_length))
   {
      effective_cp_length = (port012_use+2) / 3; // +2 to round up to the next cycle
      reason = DynamicMicroOpBoomV1::UOP_PORT012;
   }

   if (update_reason && effective_cp_length > critical_path_length)
   {
      LOG_ASSERT_ERROR(reason != DynamicMicroOpBoomV1::UOP_PORT_SIZE, "There should be a reason for the cp extention, but there isn't");
      m_cpContrByPort[reason] += 1000000 * (effective_cp_length - critical_path_length) / effective_cp_length;
   }

   return effective_cp_length;
}
