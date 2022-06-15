/*
 * This file is covered under the Interval Academic License, see LICENCE.academic
 */

#include "interval_contention_cortex_a53.h"
#include "core.h"
#include "stats.h"

IntervalContentionCortexA53::IntervalContentionCortexA53(const Core *core, const CoreModel *core_model)
   : m_core_model(core_model)
{
   for(unsigned int i = 0; i < DynamicMicroOpCortexA53::UOP_PORT_SIZE; ++i)
   {
      m_cpContrByPort[i] = 0;
      String name = String("cpContr_") + DynamicMicroOpCortexA53::PortTypeString((DynamicMicroOpCortexA53::uop_port_t)i);
      registerStatsMetric("interval_timer", core->getId(), name, &(m_cpContrByPort[i]));
   }
}

void IntervalContentionCortexA53::clearFunctionalUnitStats()
{
   for(unsigned int i = 0; i < (unsigned int)DynamicMicroOpCortexA53::UOP_PORT_SIZE; ++i)
   {
      m_count_byport[i] = 0;
   }
}

void IntervalContentionCortexA53::addFunctionalUnitStats(const DynamicMicroOp *uop)
{
   m_count_byport[uop->getCoreSpecificInfo<DynamicMicroOpCortexA53>()->getPort()]++;
}

void IntervalContentionCortexA53::removeFunctionalUnitStats(const DynamicMicroOp *uop)
{
   m_count_byport[uop->getCoreSpecificInfo<DynamicMicroOpCortexA53>()->getPort()]--;
}

uint64_t IntervalContentionCortexA53::getEffectiveCriticalPathLength(uint64_t critical_path_length, bool update_reason)
{
   //DynamicMicroOpCortexA53::uop_port_t reason = DynamicMicroOpCortexA53::UOP_PORT_SIZE;
   uint64_t effective_cp_length = critical_path_length;

   // For the standard ports, check if we have exceeded our execution limit
/*   for (unsigned int i = 0 ; i < DynamicMicroOpCortexA53::UOP_PORT_SIZE ; i++)
   {
      // Skip shared ports
      if (i != DynamicMicroOpCortexA53::UOP_PORT015
         && i != DynamicMicroOpCortexA53::UOP_PORT05
         && effective_cp_length < m_count_byport[i]
      )
      {
         effective_cp_length = m_count_byport[i];
         reason = (DynamicMicroOpCortexA53::uop_port_t)i;
      }
   }
   // Check shared port usage
   uint64_t port05_use = m_count_byport[DynamicMicroOpCortexA53::UOP_PORT0] + m_count_byport[DynamicMicroOpCortexA53::UOP_PORT5]
                       + m_count_byport[DynamicMicroOpCortexA53::UOP_PORT05];
   if (port05_use > (2*effective_cp_length))
   {
      effective_cp_length = (port05_use+1) / 2; // +1 to round up to the next cycle
      reason = DynamicMicroOpCortexA53::UOP_PORT05;
   }
   uint64_t port015_use = m_count_byport[DynamicMicroOpCortexA53::UOP_PORT0] + m_count_byport[DynamicMicroOpCortexA53::UOP_PORT1]
                        + m_count_byport[DynamicMicroOpCortexA53::UOP_PORT5] + m_count_byport[DynamicMicroOpCortexA53::UOP_PORT015];
   if (port015_use > (3*effective_cp_length))
   {
      effective_cp_length = (port015_use+2) / 3; // +2 to round up to the next cycle
      reason = DynamicMicroOpCortexA53::UOP_PORT015;
   }

   if (update_reason && effective_cp_length > critical_path_length)
   {
      LOG_ASSERT_ERROR(reason != DynamicMicroOpCortexA53::UOP_PORT_SIZE, "There should be a reason for the cp extention, but there isn't");
      m_cpContrByPort[reason] += 1000000 * (effective_cp_length - critical_path_length) / effective_cp_length;
   }*/

   return effective_cp_length;
}
