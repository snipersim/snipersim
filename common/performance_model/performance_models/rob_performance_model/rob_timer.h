/*
 * This file is covered under the Interval Academic License, see LICENCE.interval
 */

#ifndef ROBTIMER_HPP_
#define ROBTIMER_HPP_

#include "interval_timer.h"
#include "rob_contention.h"
#include "stats.h"

#include <deque>

class RobTimer
{
private:
   class RobEntry
   {
      private:
         static const size_t MAX_INLINE_DEPENDANTS = 8;
         size_t numInlineDependants;
         RobEntry* inlineDependants[MAX_INLINE_DEPENDANTS];
         std::vector<RobEntry*> *vectorDependants;
         std::vector<uint64_t> addressProducers;

      public:
         void init(DynamicMicroOp *uop, UInt64 sequenceNumber);
         void free();

         void addDependant(RobEntry* dep);
         uint64_t getNumDependants() const;
         RobEntry* getDependant(size_t idx) const;

         void addAddressProducer(UInt64 sequenceNumber) { addressProducers.push_back(sequenceNumber); }
         UInt64 getNumAddressProducers() const { return addressProducers.size(); }
         UInt64 getAddressProducer(size_t idx) const { return addressProducers.at(idx); }

         DynamicMicroOp *uop;
         SubsecondTime dispatched;
         SubsecondTime ready;    // Once all dependencies are resolved, cycle number that this uop becomes ready for issue
         SubsecondTime readyMax; // While some but not all dependencies are resolved, keep the time of the latest known resolving dependency
         SubsecondTime addressReady;
         SubsecondTime addressReadyMax;
         SubsecondTime issued;
         SubsecondTime done;
   };

   const uint64_t dispatchWidth;
   const uint64_t commitWidth;
   const uint64_t windowSize;
   const uint64_t rsEntries;
   const uint64_t misprediction_penalty;
   const bool m_store_to_load_forwarding;
   const bool m_no_address_disambiguation;
   const bool inorder;

   Core *m_core;

   typedef CircularQueue<RobEntry> Rob;
   Rob rob;
   uint64_t m_num_in_rob;
   uint64_t m_rs_entries_used;
   RobContention *m_rob_contention;

   ComponentTime now;
   SubsecondTime frontend_stalled_until;
   bool in_icache_miss;
   SubsecondTime last_store_done;
   ContentionModel load_queue;
   ContentionModel store_queue;

   uint64_t nextSequenceNumber;
   bool will_skip;
   SubsecondTime time_skipped;

   RegisterDependencies* const registerDependencies;
   MemoryDependencies* const memoryDependencies;

   int addressMask;

   UInt64 m_uop_type_count[MicroOp::UOP_SUBTYPE_SIZE];
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

   SubsecondTime m_outstandingLongLatencyInsns;
   SubsecondTime m_outstandingLongLatencyCycles;
   SubsecondTime m_lastAccountedMemoryCycle;

   uint64_t m_loads_count;
   SubsecondTime m_loads_latency;
   uint64_t m_stores_count;
   SubsecondTime m_stores_latency;

   uint64_t m_totalProducerInsDistance;
   uint64_t m_totalConsumers;
   std::vector<uint64_t> m_producerInsDistance;

   PerformanceModel *perf;

#if DEBUG_IT_INSN_PRINT
   FILE *m_insn_log;
#endif

   uint64_t m_numMfenceInsns;
   uint64_t m_totalMfenceLatency;

   // CPI stacks
   SubsecondTime m_cpiBase;
   SubsecondTime m_cpiBranchPredictor;
   SubsecondTime m_cpiSerialization;
   SubsecondTime m_cpiRSFull;

   std::vector<SubsecondTime> m_cpiInstructionCache;
   std::vector<SubsecondTime> m_cpiDataCache;

   SubsecondTime *m_cpiCurrentFrontEndStall;

   const bool m_mlp_histogram;
   static const unsigned int MAX_OUTSTANDING = 32;
   std::vector<std::vector<SubsecondTime> > m_outstandingLoads;
   std::vector<SubsecondTime> m_outstandingLoadsAll;

   RobEntry *findEntryBySequenceNumber(UInt64 sequenceNumber);
   SubsecondTime* findCpiComponent();
   void countOutstandingMemop(SubsecondTime time);
   void printRob();

   void execute(uint64_t& instructionsExecuted, SubsecondTime& latency);
   SubsecondTime doDispatch(SubsecondTime **cpiComponent);
   SubsecondTime doIssue();
   SubsecondTime doCommit(uint64_t& instructionsExecuted);

   void issueInstruction(uint64_t idx, SubsecondTime &next_event);

public:

   RobTimer(Core *core, PerformanceModel *perf, const CoreModel *core_model, int misprediction_penalty, int dispatch_width, int window_size);
   ~RobTimer();

   boost::tuple<uint64_t,SubsecondTime> simulate(const std::vector<DynamicMicroOp*>& insts);
   void synchronize(SubsecondTime time);
};

#endif /* ROBTIMER_H_ */
