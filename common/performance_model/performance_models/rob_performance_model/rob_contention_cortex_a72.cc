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
   /* The issue pipelines, by kind.  The defaults are the A72's (Cortex-A72
      Software Optimization Guide, UAN0016A, section 2.1): one branch, two
      integer, one integer multi-cycle, two FP/ASIMD, one load and one store
      pipeline.  A wider core built on this model (config/core_configs/
      cortex_a720, cortex_x4, oryon_xelite) gives its own counts;
      load_store bounds loads and stores together, for cores whose
      load/store pipelines take either. */
   const String k = "perf_model/core/cortex_a72_ports/";
   m_branch = Sim()->getCfg()->getIntDefault(k + "branch", 1);
   m_integer = Sim()->getCfg()->getIntDefault(k + "integer", 2);
   m_multi = Sim()->getCfg()->getIntDefault(k + "integer_multi", 1);
   m_fp = Sim()->getCfg()->getIntDefault(k + "fp_asimd", 2);
   m_load = Sim()->getCfg()->getIntDefault(k + "load", 1);
   m_store = Sim()->getCfg()->getIntDefault(k + "store", 1);
   m_memory = Sim()->getCfg()->getIntDefault(k + "load_store", m_load + m_store);
   /* On the A72 the multi-cycle pipeline takes only multi-cycle µOPs; on
      later cores (the A720's and X4's "Integer Single/Multi-cycle" pipelines)
      it also takes single-cycle ALU µOPs */
   m_alu_on_multi = Sim()->getCfg()->getBoolDefault(k + "alu_on_multi", false);
   LOG_ASSERT_ERROR(m_branch && m_integer && m_multi && m_fp && m_load && m_store && m_memory,
                    "perf_model/core/cortex_a72_ports: every kind of pipeline needs at least one");
}

void RobContentionCortexA72::initCycle(SubsecondTime now)
{
   m_now.setElapsedTime(now);
   n_branch = n_integer = n_multi = n_fp = n_load = n_store = 0;
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

   // What the µOP needs this cycle: a pipeline of one kind, and for the
   // paired kinds an integer pipeline as well
   bool branch = false, integer = false, multi = false, fp = false, load = false, store = false;
   switch (uop_port)
   {
      case DynamicMicroOpCortexA72::UOP_PORT0:    branch = true; break;
      case DynamicMicroOpCortexA72::UOP_PORT0_12: branch = integer = true; break;
      case DynamicMicroOpCortexA72::UOP_PORT12:   integer = true; break;
      case DynamicMicroOpCortexA72::UOP_PORT3:    multi = true; break;
      case DynamicMicroOpCortexA72::UOP_PORT12_3: multi = integer = true; break;
      case DynamicMicroOpCortexA72::UOP_PORT4:
      case DynamicMicroOpCortexA72::UOP_PORT5:    fp = true; break;
      case DynamicMicroOpCortexA72::UOP_PORT6:    load = true; break;
      case DynamicMicroOpCortexA72::UOP_PORT6_12: load = integer = true; break;
      case DynamicMicroOpCortexA72::UOP_PORT7:    store = true; break;
      case DynamicMicroOpCortexA72::UOP_PORT7_12: store = integer = true; break;
      default: break;
   }
   if (integer && !multi && n_integer >= m_integer && m_alu_on_multi && n_multi < m_multi)
   {
      integer = false;     // the ALU µOP takes a free single/multi-cycle pipeline
      multi = true;
   }
   if ((branch && n_branch >= m_branch) || (integer && n_integer >= m_integer) || (multi && n_multi >= m_multi)
       || (fp && n_fp >= m_fp) || (load && n_load >= m_load) || (store && n_store >= m_store)
       || ((load || store) && n_load + n_store >= m_memory))
      return false;

   // ALU contention
   if (DynamicMicroOpCortexA72::uop_alu_t alu = core_uop_info->getAlu())
   {
      if (alu_used_until[alu] > m_now)
         return false;
   }

   n_branch += branch; n_integer += integer; n_multi += multi;
   n_fp += fp; n_load += load; n_store += store;
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
   // The dispatch width bounds issue before the ports clog
   return false;
}
