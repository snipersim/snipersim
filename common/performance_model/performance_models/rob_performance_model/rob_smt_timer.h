/*
 * This file is covered under the Interval Academic License, see LICENCE.interval
 */

#ifndef RobSmtTimer_HPP_
#define RobSmtTimer_HPP_

#include "interval_timer.h"
#include "smt_timer.h"
#include "rob_contention.h"

#include <deque>

class RobSmtTimer : public SmtTimer {
private:
   class RobEntry {
      private:
         static const size_t MAX_INLINE_DEPENDANTS = 8;
         size_t numInlineDependants;
         RobEntry* inlineDependants[MAX_INLINE_DEPENDANTS];
         std::vector<RobEntry*> *vectorDependants;

         static const size_t MAX_ADDRESS_PRODUCERS = 4;
         size_t numAddressProducers;
         uint64_t addressProducers[MAX_ADDRESS_PRODUCERS];

      public:
         void init(DynamicMicroOp *uop, UInt64 sequenceNumber);
         void free();

         void addDependant(RobEntry* dep);
         uint64_t getNumDependants() const;
         RobEntry* getDependant(size_t idx) const;

         void addAddressProducer(uint64_t sequenceNumber)
         {
            LOG_ASSERT_ERROR(numAddressProducers < MAX_ADDRESS_PRODUCERS, "Too many address producers, increase MAX_ADDRESS_PRODUCERS(%d)", MAX_ADDRESS_PRODUCERS);
            addressProducers[numAddressProducers++] = sequenceNumber;
         }
         size_t getNumAddressProducers() const { return numAddressProducers; }
         uint64_t getAddressProducer(size_t idx) const { return addressProducers[idx]; }

         DynamicMicroOp *uop;
         SubsecondTime dispatched;
         SubsecondTime ready;    // Once all dependencies are resolved, cycle number that this uop becomes ready for issue
         SubsecondTime readyMax; // While some but not all dependencies are resolved, keep the time of the latest known resolving dependency
         SubsecondTime addressReady;
         SubsecondTime addressReadyMax;
         SubsecondTime issued;
         SubsecondTime done;
   };
   typedef CircularQueue<RobEntry> Rob;

   class RobThread {
      public:
         Core *core;

         SubsecondTime now;
         uint64_t instrs;
         uint64_t instrs_returned;

         Rob rob;
         uint64_t m_num_in_rob;
         uint64_t nextSequenceNumber;

         SubsecondTime frontend_stalled_until;
         bool in_icache_miss;

         SubsecondTime next_event;

         RegisterDependencies* const registerDependencies;
         MemoryDependencies* const memoryDependencies;

         SubsecondTime *m_cpiCurrentFrontEndStall;

         std::vector<std::vector<SubsecondTime> > m_outstandingLoads;
         std::vector<SubsecondTime> m_outstandingLoadsAll;

         UInt64 m_uop_type_count[MicroOp::UOP_SUBTYPE_SIZE];
         UInt64 m_uops_total;
         UInt64 m_uops_x87;
         UInt64 m_uops_pause;

         // CPI stacks
         SubsecondTime m_cpiBase;
         SubsecondTime m_cpiSMT;
         SubsecondTime m_cpiIdle;
         SubsecondTime m_cpiBranchPredictor;
         SubsecondTime m_cpiSerialization;
         SubsecondTime m_cpiRSFull;

         std::vector<SubsecondTime> m_cpiInstructionCache;
         std::vector<SubsecondTime> m_cpiDataCache;

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

         RobThread(Core *core, int window_size);
         ~RobThread();
   };

   const uint64_t dispatchWidth;
   const uint64_t commitWidth;
   const uint64_t windowSize;    // total ROB size
   const uint64_t rsEntries;
   uint64_t currentWindowSize;   // current per-thread window size
   const uint64_t misprediction_penalty;
   const bool m_store_to_load_forwarding;
   const bool m_no_address_disambiguation;
   const bool inorder;
   const bool windowRepartition;
   const bool simultaneousIssue;

   std::vector<RobThread *> m_rob_threads;
   RobContention *m_rob_contention;

   UInt8 dispatch_thread;
   UInt8 issue_thread;

   ComponentTime now;
   SubsecondTime last_store_done;
   ContentionModel load_queue;
   ContentionModel store_queue;
   uint64_t m_rs_entries_used;

   bool will_skip;
   SubsecondTime time_skipped;

   int addressMask;

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

   PerformanceModel *perf;

#if DEBUG_IT_INSN_PRINT
   FILE *m_insn_log;
#endif

   uint64_t m_numMfenceInsns;
   uint64_t m_totalMfenceLatency;

   const bool m_mlp_histogram;
   static const unsigned int MAX_OUTSTANDING = 32;

   void setDependencies(smtthread_id_t thread_id, RobEntry *entry);
   void setStoreAddressProducers(smtthread_id_t thread_id, RobEntry *entry, uint64_t lowestValidSequenceNumber);
   RobEntry *findEntryBySequenceNumber(smtthread_id_t thread_num, UInt64 sequenceNumber);
   void countOutstandingMemop(smtthread_id_t thread_num, SubsecondTime time);
   void printRob(smtthread_id_t thread_num);
   void printRob();

   SubsecondTime executeCycle();
   SubsecondTime doDispatch();
   SubsecondTime doIssue();
   SubsecondTime doCommit();

   bool canExecute(smtthread_id_t thread_num);
   bool canExecute();
   bool tryDispatch(smtthread_id_t thread_num, SubsecondTime &next_event);
   SubsecondTime* findCpiComponent(smtthread_id_t thread_num);
   bool tryIssue(smtthread_id_t thread_num, SubsecondTime &next_event, bool would_have_skipped = false);
   void issueInstruction(smtthread_id_t thread_num, uint64_t idx, SubsecondTime &next_event);

   void computeCurrentWindowsize();

public:
   RobSmtTimer(int num_threads, Core *core, PerformanceModel *perf, const CoreModel *core_model, int misprediction_penalty, int dispatch_width, int window_size);
   virtual ~RobSmtTimer();

   virtual void initializeThread(smtthread_id_t thread_num);
   virtual uint64_t threadNumSurplusInstructions(smtthread_id_t thread_num);
   virtual bool threadHasEnoughInstructions(smtthread_id_t thread_num);
   virtual void notifyNumActiveThreadsChange();

   virtual void pushInstructions(smtthread_id_t thread_id, const std::vector<DynamicMicroOp*>& insts);
   virtual boost::tuple<uint64_t,SubsecondTime> returnLatency(smtthread_id_t thread_id);

   virtual void execute();
   virtual void synchronize(smtthread_id_t thread_id, SubsecondTime time);
};

#endif /* RobSmtTimer_H_ */
