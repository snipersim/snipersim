/*
 * This file is covered under the Interval Academic License, see LICENCE.interval
 */

#include "interval_timer.h"
#include "tools.h"
#include "core_model.h"
#include "stats.h"
#include "core_manager.h"
#include "itostr.h"
#if DEBUG_IT_INSN_PRINT
# include "performance_model.h"
# include "micro_op.h"
#endif
#include "instruction.h"
#include "loop_tracer.h"
#include "config.hpp"
#include "utils.h"

#include <algorithm>
#include <iostream>
#include <cstdio>

IntervalTimer::IntervalTimer(
         Core *core, PerformanceModel *_perf, const CoreModel *core_model,
         int misprediction_penalty,
         int dispatch_width,
         int window_size,
         bool do_functional_unit_contention)
      : m_core(core)
      , m_core_model(core_model)
      , m_dispatch_width(dispatch_width)
      , m_branch_misprediction_penalty(misprediction_penalty)
      , m_remaining_dispatch_bandwidth(0)
      , m_max_store_completion_time(0)
      , m_max_load_completion_time(0)
      , m_loadstore_contention("interval_timer.loadstore_contention", core->getId(),
           Sim()->getCfg()->getIntArray("perf_model/core/interval_timer/num_outstanding_loadstores", core->getId()))
      , m_windows(new Windows(window_size, do_functional_unit_contention, core, core_model))
      , m_perf_model(_perf)
      , m_frequency_domain(core->getDvfsDomain())
      , m_loop_tracer(LoopTracer::createLoopTracer(core))
{

   // Granularity of memory dependencies, in bytes
   UInt64 mem_gran = Sim()->getCfg()->getIntArray("perf_model/core/interval_timer/memory_dependency_granularity", core->getId());
   LOG_ASSERT_ERROR(isPower2(mem_gran), "memory_dependency_granularity needs to be a power of 2. [%u]", mem_gran);
   m_mem_dep_mask = ~(mem_gran - 1);

   // Granularity of memory dependencies on long-latency loads, in bytes
   UInt64 lll_gran = Sim()->getCfg()->getIntArray("perf_model/core/interval_timer/lll_dependency_granularity", core->getId());
   LOG_ASSERT_ERROR(isPower2(lll_gran), "lll_dependency_granularity needs to be a power of 2. [%u]", lll_gran);
   m_lll_dep_mask = ~(lll_gran - 1);

   for(int i = 0; i < MicroOp::UOP_SUBTYPE_SIZE; ++i)
   {
      m_uop_type_count[i] = 0;
      registerStatsMetric("interval_timer", core->getId(), String("uop_") + MicroOp::getSubtypeString(MicroOp::uop_subtype_t(i)), &m_uop_type_count[i]);
   }

   m_uops_total = 0;
   m_uops_x87 = 0;
   m_uops_pause = 0;

   registerStatsMetric("interval_timer", core->getId(), "uops_total", &m_uops_total);
   registerStatsMetric("interval_timer", core->getId(), "uops_x87", &m_uops_x87);
   registerStatsMetric("interval_timer", core->getId(), "uops_pause", &m_uops_pause);

   m_numICacheOverlapped = 0;
   m_numBPredOverlapped = 0;
   m_numDCacheOverlapped = 0;

   registerStatsMetric("interval_timer", core->getId(), "numICacheOverlapped", &m_numICacheOverlapped);
   registerStatsMetric("interval_timer", core->getId(), "numBPredOverlapped", &m_numBPredOverlapped);
   registerStatsMetric("interval_timer", core->getId(), "numDCacheOverlapped", &m_numDCacheOverlapped);

   m_numLongLatencyLoads = 0;
   m_numTotalLongLatencyLoadLatency = 0;

   registerStatsMetric("interval_timer", core->getId(), "numLongLatencyLoads", &m_numLongLatencyLoads);
   registerStatsMetric("interval_timer", core->getId(), "numTotalLongLatencyLoadLatency", &m_numTotalLongLatencyLoadLatency);

   m_numSerializationInsns = 0;
   m_totalSerializationLatency = 0;

   registerStatsMetric("interval_timer", core->getId(), "numSerializationInsns", &m_numSerializationInsns);
   registerStatsMetric("interval_timer", core->getId(), "totalSerializationLatency", &m_totalSerializationLatency);

   m_totalHiddenDCacheLatency = 0;
   registerStatsMetric("interval_timer", core->getId(), "totalHiddenDCacheLatency", &m_totalHiddenDCacheLatency);

   m_totalHiddenLongerDCacheLatency = 0;
   m_numHiddenLongerDCacheLatency = 0;
   registerStatsMetric("interval_timer", core->getId(), "totalHiddenLongerDCacheLatency", &m_totalHiddenLongerDCacheLatency);
   registerStatsMetric("interval_timer", core->getId(), "numHiddenLongerDCacheLatency", &m_numHiddenLongerDCacheLatency);

   m_outstandingLongLatencyCycles = 0;
   m_outstandingLongLatencyInsns = 0;
   m_lastAccountedMemoryCycle = 0;

   registerStatsMetric("interval_timer", core->getId(), "outstandingLongLatencyInsns", &m_outstandingLongLatencyInsns);
   registerStatsMetric("interval_timer", core->getId(), "outstandingLongLatencyCycles", &m_outstandingLongLatencyCycles);

#if DEBUG_IT_INSN_PRINT
   String insn_filename;
   insn_filename = "sim.timer_insn_log." + itostr(core->getId());
   insn_filename = Sim()->getConfig()->formatOutputFileName(insn_filename);
   m_insn_log = std::fopen(insn_filename.c_str(), "w");
#endif

   m_numMfenceInsns = 0;
   m_totalMfenceLatency = 0;

   registerStatsMetric("interval_timer", core->getId(), "numMfenceInsns", &m_numMfenceInsns);
   registerStatsMetric("interval_timer", core->getId(), "totalMfenceLatency", &m_totalMfenceLatency);

   m_cpiBase = SubsecondTime::Zero();
   m_cpiBranchPredictor = SubsecondTime::Zero();
   m_cpiSerialization = SubsecondTime::Zero();
   m_cpiLongLatency = SubsecondTime::Zero();

   registerStatsMetric("interval_timer", core->getId(), "cpiBase", &m_cpiBase);
   registerStatsMetric("interval_timer", core->getId(), "cpiBranchPredictor", &m_cpiBranchPredictor);
   registerStatsMetric("interval_timer", core->getId(), "cpiSerialization", &m_cpiSerialization);
   registerStatsMetric("interval_timer", core->getId(), "cpiLongLatency", &m_cpiLongLatency);

   m_cpiInstructionCache.resize(HitWhere::NUM_HITWHERES, SubsecondTime::Zero());
   for (int h = HitWhere::WHERE_FIRST ; h < HitWhere::NUM_HITWHERES ; h++)
   {
      if (HitWhereIsValid((HitWhere::where_t)h))
      {
         String name = "cpiInstructionCache" + String(HitWhereString((HitWhere::where_t)h));
         registerStatsMetric("interval_timer", core->getId(), name, &(m_cpiInstructionCache[h]));
      }
   }
   m_cpiDataCache.resize(HitWhere::NUM_HITWHERES, SubsecondTime::Zero());
   for (int h = HitWhere::WHERE_FIRST ; h < HitWhere::NUM_HITWHERES ; h++)
   {
      if (HitWhereIsValid((HitWhere::where_t)h))
      {
         String name = "cpiDataCache" + String(HitWhereString((HitWhere::where_t)h));
         registerStatsMetric("interval_timer", core->getId(), name, &(m_cpiDataCache[h]));
      }
   }

   for (int i = 0 ; i < STOP_DISPATCH_SIZE ; i++ )
   {
      m_cpiBaseStopDispatch[i] = 0;
      String name = "detailed-cpiBase-" + StopDispatchReasonString((StopDispatchReason)i);
      registerStatsMetric("interval_timer", core->getId(), name, &(m_cpiBaseStopDispatch[i]));
   }

   for(unsigned int i = 0; i < (unsigned int)CPCONTR_TYPE_SIZE; ++i)
   {
      m_cpContrByType[i] = 0;
      String name = "cpContr_" + CpContrTypeString((CpContrType)i);
      registerStatsMetric("interval_timer", core->getId(), name, &(m_cpContrByType[i]));
   }
}

IntervalTimer::~IntervalTimer()
{
   free();
}

void IntervalTimer::free()
{
   if (m_windows)
   {
      delete m_windows;
      if (m_loop_tracer)
      {
         delete m_loop_tracer;
      }
#if DEBUG_IT_INSN_PRINT
      if (m_insn_log)
      {
         std::fclose(m_insn_log);
      }
#endif
      m_windows = NULL;
   }
}

// Simulate a collection of micro-ops and report the number of instructions executed and the latency
boost::tuple<uint64_t,uint64_t> IntervalTimer::simulate(const std::vector<DynamicMicroOp*>& insts)
{
   uint64_t total_instructions_executed = 0, total_latency = 0;

   for (std::vector<DynamicMicroOp*>::const_iterator i = insts.begin() ; i != insts.end(); ++i )
   {
      if ((*i)->isSquashed())
      {
         delete *i;
         continue;
      }

      // Enforce memory dependency granularity
      if ((*i)->getMicroOp()->isLoad() || (*i)->getMicroOp()->isStore())
      {
         Memory::Access addr = (*i)->getAddress();
         addr.address &= m_mem_dep_mask;
         (*i)->setAddress(addr);
      }

      m_windows->add(*i);
      m_uop_type_count[(*i)->getMicroOp()->getSubtype()]++;
      m_uops_total++;
      if ((*i)->getMicroOp()->isX87()) m_uops_x87++;
      if ((*i)->getMicroOp()->isPause()) m_uops_pause++;

      if (m_uops_total > 10000 && m_uops_x87 > m_uops_total / 20)
      {
         LOG_PRINT_WARNING_ONCE("Significant fraction of x87 instructions encountered, accuracy will be low. Compile without -mno-sse2 -mno-sse3 to avoid these instructions.");
      }

      // Only dispatch instructions from the window when it is full
      // It needs to be full so that we can walk the window to find independent misses
      while (m_windows->wIsFull())
      {
         uint64_t instructions_executed, latency;
         boost::tie(instructions_executed, latency) = dispatchWindow();
         total_instructions_executed += instructions_executed;
         total_latency += latency;
      }
   }

   return boost::tuple<uint64_t,uint64_t>(total_instructions_executed, total_latency);
}

boost::tuple<uint64_t, uint64_t> IntervalTimer::dispatchWindow() {
   uint64_t instructions_executed = 0;
   uint64_t latency = 0;
   uint64_t micro_ops_executed = 0;

   uint32_t dispatch_rate = calculateCurrentDispatchRate();

   StopDispatchReason continue_dispatching = STOP_DISPATCH_NO_REASON;

   SubsecondTime micro_op_period = m_frequency_domain->getPeriod(); // Approximate the current cycle count if we don't find a MicroOp

   // Instruction dispatch
   // micro_ops_executed must be < dispatch_rate.  micro_ops_executed <= dispatch_rate will cause significant speedups (see calculateCurrentDispatchRate())
   while ( (!m_windows->wIsEmpty()) && (instructions_executed < m_dispatch_width) && (micro_ops_executed < dispatch_rate) && (continue_dispatching == STOP_DISPATCH_NO_REASON))
   {
      Windows::WindowEntry& micro_op = m_windows->getInstructionToDispatch();

      uint64_t instruction_latency = dispatchInstruction(micro_op, continue_dispatching);
      latency += instruction_latency;
      m_windows->dispatchInstruction();

      micro_ops_executed++;
      if (micro_op.getDynMicroOp()->isLast())
      {
         instructions_executed++;
      }

      if (m_loop_tracer)
      {
         m_loop_tracer->issue(micro_op.getDynMicroOp(), micro_op.getExecTime(), micro_op.getExecTime());
      }

#if DEBUG_IT_INSN_PRINT
      if (latency > 16)
      {
         uint64_t insn_count = m_perf_model->getInstructionCount();
         uint64_t cycle_count = m_perf_model->getCycleCount();
# ifdef ENABLE_MICROOP_STRINGS
         const char *opcode_name = micro_op.getMicroOp()->getInstructionOpcodeName().c_str();
# else
         const char *opcode_name = "Unknown";
# endif
         fprintf(m_insn_log, "[%ld,%ld] %s latency=%d\n", cycle_count, insn_count, opcode_name, instruction_latency);
      }
#endif

      micro_op_period = micro_op.getDynMicroOp()->getPeriod();
   }

   // The minimum latency for dispatching these micro-ops is 1 cycle
   // Detect the microarchitectural limiting factors for this cycle for cpi accounting
   if (latency == 0)
   {
      if (m_windows->wIsEmpty())
      {
         continue_dispatching = ADD_STOP_DISPATCH_REASON(STOP_DISPATCH_WINDOW_EMPTY, continue_dispatching);
      }
      if (instructions_executed >= m_dispatch_width)
      {
         continue_dispatching = ADD_STOP_DISPATCH_REASON(STOP_DISPATCH_DISPATCH_WIDTH, continue_dispatching);
      }
      if (micro_ops_executed >= dispatch_rate)
      {
         continue_dispatching = ADD_STOP_DISPATCH_REASON(STOP_DISPATCH_DISPATCH_RATE, continue_dispatching);
         // Each time we have to stop because the critical path tells us to:
         //   Add the current critical path components to their corresponding total
         //   If the critical path got extended due to issue contention, add that separately
         int critical_path_length = m_windows->getCriticalPathLength();
         int effective_cp_length = m_windows->getEffectiveCriticalPathLength(critical_path_length, true);
         for(unsigned int i = 0; i < (unsigned int)CPCONTR_TYPE_SIZE; ++i)
            m_cpContrByType[i] += m_windows->getCpContrFraction((CpContrType)i, effective_cp_length);
         #if 0
         printf("cpContr: ");
         for(unsigned int i = 0; i < (unsigned int)CPCONTR_TYPE_SIZE; ++i)
            printf("%s %3lu  ", CpContrTypeString((CpContrType)i).c_str(), m_windows->m_cpcontr_bytype[i]);
         printf("/ total %4lu\n", m_windows->m_cpcontr_total);
         #endif
      }

      latency = 1;
      // Update CPI-stacks
      m_cpiBase += 1 * micro_op_period;
      m_cpiBaseStopDispatch[continue_dispatching] += 1;
   }

   return boost::tuple<uint64_t,uint64_t>(instructions_executed, latency);
}

uint32_t IntervalTimer::calculateCurrentDispatchRate() {

   int critical_path_length = m_windows->getCriticalPathLength();

   uint32_t dispatch_rate;

   if (critical_path_length > 0)
   {
      FixedPoint ipc = FixedPoint(m_windows->getOldWindowLength()) / m_windows->getEffectiveCriticalPathLength(critical_path_length, false) + m_remaining_dispatch_bandwidth;
      dispatch_rate = FixedPoint::floor(ipc);
      m_remaining_dispatch_bandwidth = (dispatch_rate < m_dispatch_width) ? (ipc - dispatch_rate) : 0;
   }
   else
   {
      dispatch_rate = m_dispatch_width;
      m_remaining_dispatch_bandwidth = 0;
   }

   return dispatch_rate;
}

void IntervalTimer::issueMemOp(Windows::WindowEntry& micro_op)
{
   // Issue memory operations to the memory hierarchy.
   // This function is called:
   // - not at all if all memory operations are issued at fetch (perf_model/core/interval_timer/issue_memops_at_dispatch == false)
   // - from blockWindow for overlapping accesses
   // - from dispatchInstruction otherwise
   if ((micro_op.getMicroOp()->isLoad() || micro_op.getMicroOp()->isStore())
      && micro_op.getDynMicroOp()->getDCacheHitWhere() == HitWhere::UNKNOWN)
   {
      MemoryResult res = m_core->accessMemory(
         Core::NONE,
         micro_op.getMicroOp()->isLoad() ? Core::READ : Core::WRITE,
         micro_op.getDynMicroOp()->getAddress().address,
         NULL,
         micro_op.getMicroOp()->getMemoryAccessSize(),
         Core::MEM_MODELED_RETURN,
         micro_op.getMicroOp()->getInstruction() ? micro_op.getMicroOp()->getInstruction()->getAddress() : static_cast<uint64_t>(NULL)
      );
      uint64_t latency = SubsecondTime::divideRounded(res.latency, m_core->getDvfsDomain()->getPeriod());
      micro_op.getDynMicroOp()->setExecLatency(micro_op.getDynMicroOp()->getExecLatency() + latency); // execlatency already contains bypass latency
      micro_op.getDynMicroOp()->setDCacheHitWhere(res.hit_where);
   }
}

uint64_t IntervalTimer::dispatchInstruction(Windows::WindowEntry& micro_op, StopDispatchReason& continue_dispatching)
{
   // If it's not already done, issue the memory operation
   issueMemOp(micro_op);

   uint64_t latency = 0;

   uint64_t max_producer_exec_time = getMaxProducerExecTime(micro_op);
   micro_op.maxProducer = max_producer_exec_time;
   micro_op.cphead = m_windows->getCriticalPathHead();
   micro_op.cptail = m_windows->getCriticalPathTail();

   bool icache_miss = (micro_op.getDynMicroOp()->getICacheHitWhere() != HitWhere::L1I) & (!micro_op.hasOverlapFlag(Windows::WindowEntry::ICACHE_OVERLAP));

   if (icache_miss)
   {
      uint64_t icache_latency = micro_op.getDynMicroOp()->getICacheLatency();
      latency += icache_latency;

      m_windows->clearOldWindow(micro_op.cptail + icache_latency);

      continue_dispatching = STOP_DISPATCH_ICACHE_MISS;
      // Update icache CPI-stack counters
      m_cpiInstructionCache[micro_op.getDynMicroOp()->getICacheHitWhere()] += icache_latency * micro_op.getDynMicroOp()->getPeriod();
   }

   if (micro_op.getMicroOp()->isBranch())
   {
      if (micro_op.getDynMicroOp()->isBranchMispredicted() && !micro_op.hasOverlapFlag(Windows::WindowEntry::BPRED_OVERLAP))
      {
         uint64_t bpred_latency = m_branch_misprediction_penalty + m_windows->calculateBranchResolutionLatency();
         latency += bpred_latency;

         continue_dispatching = STOP_DISPATCH_BRANCH_MISPREDICT;
         m_windows->clearOldWindow(micro_op.cptail + bpred_latency);
         // Update bpred CPI-stack counters
         m_cpiBranchPredictor += bpred_latency * micro_op.getDynMicroOp()->getPeriod();
      }
   }

   if (micro_op.getMicroOp()->isSerializing() || micro_op.getMicroOp()->isInterrupt())
   {
      uint64_t flushLatency = std::max(m_windows->getCriticalPathLength(), m_windows->getMinimalFlushLatency(m_dispatch_width));
      uint64_t serialize_latency = flushLatency + micro_op.getDynMicroOp()->getExecLatency();
      latency += serialize_latency;

      m_numSerializationInsns++;
      m_totalSerializationLatency += serialize_latency;
      m_cpiSerialization += serialize_latency * micro_op.getDynMicroOp()->getPeriod();

      micro_op.setExecTime(m_windows->getCriticalPathTail());
      m_windows->clearOldWindow(micro_op.getExecTime());
   }
   else if (micro_op.getMicroOp()->isExecute() && micro_op.getMicroOp()->isMemBarrier())
   {
      // Handle the M/L/SFENCE operations (when supplied as a single execute operation) now as a strict MFENCE
      //uint64_t current_cycle = m_windows->getCriticalPathTail();
      uint64_t cycle_to_wait_until = std::max(max_producer_exec_time,std::max(m_max_store_completion_time,m_max_load_completion_time));
      // If we have loads or stores that will complete in the future, wait for them all to complete
      // Otherwise, our critical path is longer, and we become a nop
      // This code below doesn't work.  CriticalPath is just the time that we put the last instruction in.
      // We need to use the maxProd, and we can't just add latencies, because we don't have a concept of latency
      //  in the old-window (only execution ciompletion time)
      // We just need to make sure that the memory dependencies are properly taken care of.
      //if (cycle_to_wait_until > current_cycle) {
      //   uint64_t mfence_latency = cycle_to_wait_until - current_cycle;
      //   // Keep track of the memory fence instructions and their contributed latencies
      //   m_totalMfenceLatency += mfence_latency;
      //}
      m_numMfenceInsns++;
      micro_op.setExecTime(cycle_to_wait_until + micro_op.getDynMicroOp()->getExecLatency());
      updateCriticalPath(micro_op, latency);
   }
   else
   {
      uint64_t exec_latency = micro_op.getDynMicroOp()->getExecLatency();

      if (micro_op.getMicroOp()->isLoad())
      {
         if (micro_op.hasOverlapFlag(Windows::WindowEntry::DCACHE_OVERLAP))
         {
            // do nothing for overlapped loads, as they don't add to the critical path and happen in parallel to other loads (MLP > 1)
         }
         else if (micro_op.getDynMicroOp()->isLongLatencyLoad())
         {
            uint64_t dcache_latency = exec_latency;
            // Long latency loads trump all other latencies
            uint64_t dispatch_cycle = std::max(max_producer_exec_time, m_windows->getCriticalPathHead());
            uint64_t sched_cycle = dispatch_cycle;
            uint64_t contention_exec_cycle;
            // This logic only works for LFENCEs, when marked on load uops
            if (micro_op.getMicroOp()->isMemBarrier())
               contention_exec_cycle = m_loadstore_contention.getBarrierCompletionTime(sched_cycle, dcache_latency);
            else
               contention_exec_cycle = m_loadstore_contention.getCompletionTime(sched_cycle, dcache_latency);

            // Calculate our new latency from the load contention completion time
            uint64_t reswin_extra_latency = sched_cycle - dispatch_cycle;
            uint64_t contention_extra_latency = contention_exec_cycle - sched_cycle;
            uint64_t long_latency_load_latency = reswin_extra_latency + contention_extra_latency; // dcache_latency is already taken into account in contention_extra_latency
            latency += long_latency_load_latency;

#if DEBUG_IT_INSN_PRINT
            uint64_t insn_count = m_perf_model->getInstructionCount();
            uint64_t cycle_count = m_perf_model->getCycleCount();
# ifdef ENABLE_MICROOP_STRINGS
            const char *opcode_name = micro_op.getInstructionOpcodeName().c_str();
# else
            const char *opcode_name = "Unknown";
# endif
            fprintf(m_insn_log, "[%ld,%ld] %s(", cycle_count, insn_count, opcode_name);
            if (micro_op.isLoad())
               fprintf(m_insn_log, "L");
            if (micro_op.isExecute())
               fprintf(m_insn_log, "X");
            if (micro_op.isStore())
               fprintf(m_insn_log, "S");
            fprintf(m_insn_log, ") latency=%ld (%ld+%ld(%ld))\n", latency, reswin_extra_latency, contention_extra_latency, dcache_latency);
#endif

            micro_op.setExecTime(contention_exec_cycle);

            micro_op.setExecTime(m_windows->getCriticalPathTail());
            m_windows->clearOldWindow(micro_op.getExecTime() + long_latency_load_latency);

            this->blockWindow();

            m_numLongLatencyLoads++;
            m_numTotalLongLatencyLoadLatency+=long_latency_load_latency;

            // Update dcache CPI-stack counters
            m_cpiDataCache[micro_op.getDynMicroOp()->getDCacheHitWhere()] += long_latency_load_latency * micro_op.getDynMicroOp()->getPeriod();
         }
         else
         {
            // Non long-latency-load load operations
            uint64_t dcache_latency = exec_latency;
            uint64_t dispatch_cycle = std::max(max_producer_exec_time, m_windows->getCriticalPathHead());
            uint64_t sched_cycle = dispatch_cycle;
            uint64_t contentionExecTime;
            // This logic only works for LFENCEs, when marked on load uops
            if (micro_op.getMicroOp()->isMemBarrier())
               contentionExecTime = m_loadstore_contention.getBarrierCompletionTime(sched_cycle, dcache_latency);
            else
               contentionExecTime = m_loadstore_contention.getCompletionTime(sched_cycle, dcache_latency);
            micro_op.setExecTime(contentionExecTime);
            /* doesn't block window but adds to the critical path (unless long-latency). */
            updateCriticalPath(micro_op, latency);
         }

         m_max_load_completion_time = std::max(m_max_load_completion_time, micro_op.getExecTime());

         // Compute MLP
         if (micro_op.getDynMicroOp()->isLongLatencyLoad())
         {
            uint64_t now = m_windows->getCriticalPathTail();
            uint64_t done = now + exec_latency;

            // Ins will be outstanding for until it is done. By accounting beforehand I don't need to
            // worry about fast-forwarding simulations
            m_outstandingLongLatencyInsns += exec_latency;

            // Only account for the cycles that have not yet been accounted for by other long
            // latency misses (don't account cycles twice).
            if (m_lastAccountedMemoryCycle < now)
            {
               m_lastAccountedMemoryCycle = now;
            }
            if (done > m_lastAccountedMemoryCycle)
            {
               m_outstandingLongLatencyCycles += done - m_lastAccountedMemoryCycle;
               m_lastAccountedMemoryCycle = done;
            }
         }

      } else if (micro_op.getMicroOp()->isStore()) {

         uint64_t store_latency = exec_latency;
         uint64_t dispatch_cycle = std::max(max_producer_exec_time, m_windows->getCriticalPathHead());
         uint64_t sched_cycle = dispatch_cycle;
         uint64_t bypass_latency = m_core_model->getBypassLatency(micro_op.getDynMicroOp());
         uint64_t data_ready_cycle = sched_cycle + bypass_latency + 1; // This store result will be ready to use one cycle later
         micro_op.setExecTime(data_ready_cycle); // Time the critical path calculation will use
         updateCriticalPath(micro_op, latency);

         // FIXME See Redmine issue #89: CPU forwarding logic vs. RFO store completion delaying the CPU
         uint64_t exec_time_cycle = sched_cycle + store_latency; // For future instructions that depend on this instruction's result
         m_max_store_completion_time = std::max(m_max_store_completion_time, exec_time_cycle);

         // FIXME: Don't update micro_op.setExecTime (else it will eventually affect the critical path head, once this instruction
         // goes through Windows::dispatchInstruction). If we need this time, figure out another place to put it.
         //micro_op.setExecTime(exec_time_cycle);

      } else {
         uint64_t dispatch_cycle = std::max(max_producer_exec_time, m_windows->getCriticalPathHead());
         micro_op.setExecTime(dispatch_cycle + exec_latency);

         updateCriticalPath(micro_op, latency);
      }
   }

   return latency;
}

/**
 * Move a micro_op to the old window. If it extends the critical path
 * by more than the long-latency cut-off, clear the window instead.
 */
void IntervalTimer::updateCriticalPath(Windows::WindowEntry& micro_op, uint64_t& latency)
{
   uint64_t lll = m_windows->longLatencyOperationLatency(micro_op);
   if (lll == 0)
   {
      m_windows->updateCriticalPathTail(micro_op);
   }
   else
   {
      latency += lll;
      m_cpiLongLatency += lll * micro_op.getDynMicroOp()->getPeriod();
      m_windows->clearOldWindow(micro_op.getExecTime());
   }
}

/**
 * An microOperation blocks the window: mark the micro-ops that overlap with this micro-op.
 * Start the loads that overlap with this micro-op.
 */
void IntervalTimer::blockWindow()
{
   // Calculate the overlaps for all instructions in the window.
   bool mem_barrier_pending = false;

   Windows::Iterator window_iterator = m_windows->getWindowIterator();

   Windows::WindowEntry& head = window_iterator.next(); // Returns the current head: disregard it.
   head.setIndependentMiss();

   IntPtr head_address = head.getMicroOp()->isLoad() ? head.getDynMicroOp()->getLoadAccess().phys : 0;
   head_address &= m_lll_dep_mask;

   while(window_iterator.hasNext()) {
      Windows::WindowEntry& micro_op = window_iterator.next();

      micro_op.clearDependent();
      micro_op.addOverlapFlag(Windows::WindowEntry::ICACHE_OVERLAP);
      m_numICacheOverlapped++;

      if (micro_op.getMicroOp()->isSerializing())
         break;

      if (micro_op.getMicroOp()->isMemBarrier())
         mem_barrier_pending = true;

      // Generate dependencies using dependencies
      for(uint32_t i = 0; i < micro_op.getDynMicroOp()->getDependenciesLength(); i++)
      {
         if (m_windows->windowContains(micro_op.getDynMicroOp()->getDependency(i)))
         {
            Windows::WindowEntry& dependee = m_windows->getInstruction(micro_op.getDynMicroOp()->getDependency(i));
            if (dependee.isDependent())
            {
               // Dependee depends on the long-latency load blocking the window: do not issue this uop now
               micro_op.setDataDependent();
               break;
            }
            else if (dependee.isIndependent() && dependee.getDynMicroOp()->isLongLatencyLoad())
            {
               // Our dependee is independent of the long-latency load blocking the window,
               // but it is a long-latency event by itself: do not issue this uop now
               // Since the window head is independent and long-latency,
               // this code path will also start the chain of dependent loads.
               micro_op.setDataDependent();
               break;
            }
            // else: our dependee is independent of the long-latency load blocking the window,
            // and is not a long-latency load in itself, which means it will complete under the original LLL.
            // Therefore, we can also be hidden under the long-latency load which makes us not dependent.
         }
      }

      if (micro_op.getMicroOp()->isLoad())
      {
         IntPtr address = micro_op.getDynMicroOp()->getLoadAccess().phys;
         if ((address & m_lll_dep_mask) == head_address)
         {
            // This load accesses the same cache line as the long-latency load that's blocking the ROB
            // We should be an overlapped miss, but due to the way the cache model works (instant completion),
            // we will see an L1 hit. Still, we can't complete until the miss is resolved.
            // Model this by making us dependent on the long-latency load.
            micro_op.setDataDependent();
         }
      }

      if (micro_op.getMicroOp()->isBranch() && !micro_op.isDependent())
      {
          micro_op.addOverlapFlag(Windows::WindowEntry::BPRED_OVERLAP);
          m_numBPredOverlapped++;
      }
      else if (micro_op.getMicroOp()->isLoad() && !micro_op.isDependent())
      {
         /* no previous membars & not data dependend, mark as overlapped
          * if long latency miss -> mark as independent miss
          * if short latency -> check if delayed hit and also mark as independent (secondary) miss
          */
         if (!mem_barrier_pending)
         {
            if (!micro_op.hasOverlapFlag(Windows::WindowEntry::DCACHE_OVERLAP))
            {
               uint64_t overlappedLatency = micro_op.getDynMicroOp()->getExecLatency();
               uint64_t longLatency = head.getDynMicroOp()->getExecLatency();
               m_totalHiddenDCacheLatency += overlappedLatency;

               if (overlappedLatency > longLatency)
               {
                  m_totalHiddenLongerDCacheLatency += overlappedLatency - longLatency;
                  m_numHiddenLongerDCacheLatency += 1;
               }

               micro_op.addOverlapFlag(Windows::WindowEntry::DCACHE_OVERLAP);
               m_numDCacheOverlapped++;

               // Issue the memory operation now
               issueMemOp(micro_op);

               micro_op.setIndependentMiss();
               micro_op.setExecTime(m_windows->getCriticalPathHead());
            }
         }
      }
   }
}

// Allow the use of negative times for comparisons
uint64_t IntervalTimer::getMaxProducerExecTime(Windows::WindowEntry& micro_op) {
   int64_t oldestStartTime = m_windows->getOldestInstruction().getExecTime() - m_windows->getOldestInstruction().getDynMicroOp()->getExecLatency();
   int64_t oldestFetchTime = m_windows->getOldestInstruction().getFetchTime();
   int64_t currentFetchTime = micro_op.getFetchTime();
   int64_t relativeFetchTime = currentFetchTime - oldestFetchTime;

   int64_t max_producer_exec_time = 0;

   for(uint32_t i = 0; i < micro_op.getDynMicroOp()->getDependenciesLength(); i++) {
      if (m_windows->oldWindowContains(micro_op.getDynMicroOp()->getDependency(i))) {
         Windows::WindowEntry& producer = m_windows->getInstruction(micro_op.getDynMicroOp()->getDependency(i));
         int64_t producerExecTime = producer.getExecTime();
         int64_t producerStartTime = producerExecTime - producer.getDynMicroOp()->getExecLatency();
         int64_t relativeStartTime = producerStartTime - oldestStartTime;

         if (relativeFetchTime > relativeStartTime) {
            if (relativeStartTime < 0)
               relativeStartTime = 0;
            producerExecTime += (relativeFetchTime - relativeStartTime);
         }

         max_producer_exec_time = std::max(max_producer_exec_time, producerExecTime);
      }
   }

   return max_producer_exec_time;
}

String StopDispatchReasonStringHelper(StopDispatchReason r)
{
   switch(r)
   {
   case STOP_DISPATCH_NO_REASON:
      return String("NoReason");
   case STOP_DISPATCH_WINDOW_EMPTY:
      return String("WindowEmpty");
   case STOP_DISPATCH_DISPATCH_WIDTH:
      return String("DispatchWidth");
   case STOP_DISPATCH_DISPATCH_RATE:
      return String("DispatchRate");
   case STOP_DISPATCH_ICACHE_MISS:
      return String("ICacheMiss");
   case STOP_DISPATCH_BRANCH_MISPREDICT:
      return String("BranchMispredict");
   default:
      return String("UnknownStopDispatchReason");
   }
}

String StopDispatchReasonString(StopDispatchReason r)
{
   if (r == STOP_DISPATCH_NO_REASON)
   {
      return StopDispatchReasonStringHelper(r);
   }
   String s;
   for (int i = 0 ; (0x1 << i) < STOP_DISPATCH_SIZE ; i++ )
   {
      if ( (r >> i) & 0x1 )
      {
         if (s != "")
         {
            s += "+";
         }
         s += StopDispatchReasonStringHelper((StopDispatchReason)(0x1 << i));
      }
   }
   return s;
}
