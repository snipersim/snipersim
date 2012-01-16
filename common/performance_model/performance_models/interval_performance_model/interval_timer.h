/*
 * This file is covered under the Interval Academic License, see LICENCE.interval
 */

#ifndef INTERVALTIMER_HPP_
#define INTERVALTIMER_HPP_

#include <vector>

#include "fixed_point.h"
#include "windows.h"
#include "micro_op.h"
#include "boost/tuple/tuple.hpp"
#include "core.h"
#include "contention_model.h"

#define DEBUG_IT_INSN_PRINT 0

enum StopDispatchReason {
   STOP_DISPATCH_NO_REASON = 0,
   STOP_DISPATCH_WINDOW_EMPTY = 1,
   STOP_DISPATCH_DISPATCH_WIDTH = 2,
   STOP_DISPATCH_DISPATCH_RATE = 4,
   STOP_DISPATCH_ICACHE_MISS = 8,
   STOP_DISPATCH_BRANCH_MISPREDICT = 16,
   STOP_DISPATCH_WIN_FUNCTIONAL_UNIT = 32,
   STOP_DISPATCH_SIZE = 64,
};

#define ADD_STOP_DISPATCH_REASON(_new_reason, _original_reasons) ((StopDispatchReason)(((int)_new_reason) | (int)(_original_reasons)))

String StopDispatchReasonStringHelper(StopDispatchReason r);
String StopDispatchReasonString(StopDispatchReason r);

class IntervalTimer {
public:

   IntervalTimer(Core *core, PerformanceModel *perf, int misprediction_penalty, int dispatch_width, int window_size, bool do_functional_unit_contention);
   ~IntervalTimer();

   // simulate() returns (instructions_executed, latency)
   boost::tuple<uint64_t,uint64_t> simulate(const std::vector<MicroOp>& insts);

protected:

   // dispatchWindow() returns (instructions_executed, latency)
   boost::tuple<uint64_t,uint64_t> dispatchWindow();
   uint32_t calculateCurrentDispatchRate();
   void fetchInstruction(MicroOp& instruction);
   // dispatchInstruction() returns instruction_latency
   uint64_t dispatchInstruction(MicroOp& instruction, StopDispatchReason& continueDispatching);
   void updateCriticalPath(MicroOp& microOp, uint64_t& latency);
   void blockWindow();
   uint64_t getMaxProducerExecTime(MicroOp& instruction);

private:

   // Interval model parameters
   const uint32_t m_dispatch_width;
   const uint32_t m_branch_misprediction_penalty;

   // State of previous cycle
   FixedPoint m_remaining_dispatch_bandwidth;

   // For LOCKed instructions, we need to determine the latency of all loads and stores
   // (even independent ones), as strong ordering requires that we complete them
   // before executing the instruction itself.
   uint64_t m_max_store_completion_time;
   uint64_t m_max_load_completion_time;

   ContentionModel m_loadstore_contention;

   // Window and old window data structure
   Windows *m_windows;
   PerformanceModel *m_perf_model;
   const ComponentPeriod *m_frequency_domain;

#if DEBUG_IT_INSN_PRINT
   FILE *m_insn_log;
#endif

   // Core statistics
   UInt64 m_uop_type_count[MicroOp::UOP_TYPE_SIZE];
   UInt64 m_uops_total;
   UInt64 m_uops_x87;
   UInt64 m_uops_pause;

   uint64_t m_numICacheOverlapped;
   uint64_t m_numBPredOverlapped;
   uint64_t m_numDCacheOverlapped;

   uint64_t m_numLongLatencyLoads;
   uint64_t m_numTotalLongLatencyLoadLatency;

   uint64_t m_numSerializationInsns;
   uint64_t m_totalSerializationLatency;

   uint64_t m_totalHiddenDCacheLatency;
   uint64_t m_totalHiddenLongerDCacheLatency;
   uint64_t m_numHiddenLongerDCacheLatency;

   uint64_t m_numMfenceInsns;
   uint64_t m_totalMfenceLatency;

   // CPI stack data
   SubsecondTime m_cpiBase;
   SubsecondTime m_cpiBranchPredictor;
   SubsecondTime m_cpiSerialization;
   SubsecondTime m_cpiLongLatency;

   std::vector<SubsecondTime> m_cpiInstructionCache;
   std::vector<SubsecondTime> m_cpiDataCache;

   uint64_t m_cpiBaseStopDispatch[STOP_DISPATCH_SIZE];
   uint64_t m_cpiBaseWindowStopDispatch[WIN_STOP_DISPATCH_SIZE];
   uint64_t m_cpContrByType[CPCONTR_TYPE_SIZE];

};

#endif /* INTERVALTIMER_HPP_ */
