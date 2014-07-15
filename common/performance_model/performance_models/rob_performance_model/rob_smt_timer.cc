/*
 * This file is covered under the Interval Academic License, see LICENCE.interval
 */

#include "rob_smt_timer.h"

#include "tools.h"
#include "stats.h"
#include "config.hpp"
#include "core_manager.h"
#include "thread.h"
#include "thread_manager.h"
#include "itostr.h"
#include "performance_model.h"
#include "core_model.h"
#include "rob_contention.h"
#include "instruction.h"

#include <iostream>
#include <sstream>
#include <iomanip>

// Define to get per-cycle printout of dispatch, issue, writeback stages
//#define DEBUG_PERCYCLE

// Define to not skip any cycles, but assert that the skip logic is working fine
//#define ASSERT_SKIP

RobSmtTimer::RobThread::RobThread(Core *_core, int window_size)
      : core(_core)
      , now(SubsecondTime::Zero())
      , instrs(0)
      , instrs_returned(0)
      , rob(window_size + 255)
      , m_num_in_rob(0)
      , nextSequenceNumber(0)
      , frontend_stalled_until(SubsecondTime::Zero())
      , in_icache_miss(false)
      , next_event(SubsecondTime::Zero())
      , registerDependencies(new RegisterDependencies())
      , memoryDependencies(new MemoryDependencies())
      , m_cpiCurrentFrontEndStall(&m_cpiSMT)
{
}

RobSmtTimer::RobThread::~RobThread()
{
   for(Rob::iterator it = this->rob.begin(); it != this->rob.end(); ++it)
      it->free();
}

RobSmtTimer::RobSmtTimer(
         int num_threads,
         Core *core, PerformanceModel *_perf, const CoreModel *core_model,
         int misprediction_penalty,
         int dispatch_width,
         int window_size)
      : SmtTimer(num_threads)
      , dispatchWidth(dispatch_width)
      , commitWidth(Sim()->getCfg()->getIntArray("perf_model/core/rob_timer/commit_width", core->getId()))
      , windowSize(window_size) // Assume static ROB partitioning, but redistributed once threads wake up/go asleep
      , rsEntries(Sim()->getCfg()->getIntArray("perf_model/core/rob_timer/rs_entries", core->getId()))
      , misprediction_penalty(misprediction_penalty)
      , m_store_to_load_forwarding(Sim()->getCfg()->getBoolArray("perf_model/core/rob_timer/store_to_load_forwarding", core->getId()))
      , m_no_address_disambiguation(!Sim()->getCfg()->getBoolArray("perf_model/core/rob_timer/address_disambiguation", core->getId()))
      , inorder(Sim()->getCfg()->getBoolArray("perf_model/core/rob_timer/in_order", core->getId()))
      , windowRepartition(Sim()->getCfg()->getBoolArray("perf_model/core/rob_timer/rob_repartition", core->getId()))
      , simultaneousIssue(Sim()->getCfg()->getBoolArray("perf_model/core/rob_timer/simultaneous_issue", core->getId()))
      , m_rob_contention(
           Sim()->getCfg()->getBoolArray("perf_model/core/rob_timer/issue_contention", core->getId())
           ? core_model->createRobContentionModel(core)
           : NULL)
      , dispatch_thread(0)
      , issue_thread(0)
      , now(core->getDvfsDomain())
      , last_store_done(SubsecondTime::Zero())
      , load_queue("rob_timer.load_queue", core->getId(), Sim()->getCfg()->getIntArray("perf_model/core/rob_timer/outstanding_loads", core->getId()))
      , store_queue("rob_timer.store_queue", core->getId(), Sim()->getCfg()->getIntArray("perf_model/core/rob_timer/outstanding_stores", core->getId()))
      , m_rs_entries_used(0)
      , will_skip(false)
      , time_skipped(SubsecondTime::Zero())
      , m_mlp_histogram(Sim()->getCfg()->getBoolArray("perf_model/core/rob_timer/mlp_histogram", core->getId()))
{
   computeCurrentWindowsize();

   registerStatsMetric("rob_timer", core->getId(), "time_skipped", &time_skipped);

   m_numSerializationInsns = 0;
   m_totalSerializationLatency = 0;

   registerStatsMetric("rob_timer", core->getId(), "numSerializationInsns", &m_numSerializationInsns);
   registerStatsMetric("rob_timer", core->getId(), "totalSerializationLatency", &m_totalSerializationLatency);

   m_totalHiddenDCacheLatency = 0;
   registerStatsMetric("rob_timer", core->getId(), "totalHiddenDCacheLatency", &m_totalHiddenDCacheLatency);

   m_numMfenceInsns = 0;
   m_totalMfenceLatency = 0;

   registerStatsMetric("rob_timer", core->getId(), "numMfenceInsns", &m_numMfenceInsns);
   registerStatsMetric("rob_timer", core->getId(), "totalMfenceLatency", &m_totalMfenceLatency);
}

RobSmtTimer::~RobSmtTimer()
{
   for(std::vector<RobThread *>::iterator it = m_rob_threads.begin(); it != m_rob_threads.end(); ++it)
      delete *it;
}

void RobSmtTimer::computeCurrentWindowsize()
{
   if (windowRepartition)
   {
      uint64_t num_threads_awake = 0;
      for(smtthread_id_t robthread_id = 0; robthread_id < m_threads.size(); ++robthread_id)
         if (m_threads[robthread_id]->running)
            ++num_threads_awake;
      if (num_threads_awake > 0)
         currentWindowSize = windowSize / num_threads_awake;
      else
         currentWindowSize = windowSize;
   }
   else
      currentWindowSize = windowSize / m_num_threads;
}

void RobSmtTimer::initializeThread(smtthread_id_t thread_num)
{
   assert(thread_num == m_rob_threads.size());
   Core *core = m_threads[thread_num]->core;

   RobThread *thread = new RobThread(core, windowSize);
   m_rob_threads.push_back(thread);

   for(int i = 0; i < MicroOp::UOP_SUBTYPE_SIZE; ++i)
   {
      thread->m_uop_type_count[i] = 0;
      registerStatsMetric("rob_timer", core->getId(), String("uop_") + MicroOp::getSubtypeString(MicroOp::uop_subtype_t(i)), &thread->m_uop_type_count[i]);
   }
   thread->m_uops_total = 0;
   thread->m_uops_x87 = 0;
   thread->m_uops_pause = 0;
   registerStatsMetric("rob_timer", core->getId(), "uops_total", &thread->m_uops_total);
   registerStatsMetric("rob_timer", core->getId(), "uops_x87", &thread->m_uops_x87);
   registerStatsMetric("rob_timer", core->getId(), "uops_pause", &thread->m_uops_pause);

   thread->m_cpiBase = SubsecondTime::Zero();
   thread->m_cpiIdle = SubsecondTime::Zero();
   thread->m_cpiSMT = SubsecondTime::Zero();
   thread->m_cpiBranchPredictor = SubsecondTime::Zero();
   thread->m_cpiSerialization = SubsecondTime::Zero();
   thread->m_cpiRSFull = SubsecondTime::Zero();

   registerStatsMetric("rob_timer", core->getId(), "cpiBase", &thread->m_cpiBase);
   // cpiIdle is already measured by MicroOpPerformanceModel, we just have it here as a place to dump extra cycles into.
   // Don't register a statistic for it though else cpistack.py will see it.
   registerStatsMetric("rob_timer", core->getId(), "cpiSMT", &thread->m_cpiSMT);
   registerStatsMetric("rob_timer", core->getId(), "cpiBranchPredictor", &thread->m_cpiBranchPredictor);
   registerStatsMetric("rob_timer", core->getId(), "cpiSerialization", &thread->m_cpiSerialization);
   registerStatsMetric("rob_timer", core->getId(), "cpiRSFull", &thread->m_cpiRSFull);

   thread->m_cpiInstructionCache.resize(HitWhere::NUM_HITWHERES, SubsecondTime::Zero());
   for (int h = HitWhere::WHERE_FIRST ; h < HitWhere::NUM_HITWHERES ; h++)
   {
      if (HitWhereIsValid((HitWhere::where_t)h))
      {
         String name = "cpiInstructionCache" + String(HitWhereString((HitWhere::where_t)h));
         registerStatsMetric("rob_timer", core->getId(), name, &(thread->m_cpiInstructionCache[h]));
      }
   }
   thread->m_cpiDataCache.resize(HitWhere::NUM_HITWHERES, SubsecondTime::Zero());
   for (int h = HitWhere::WHERE_FIRST ; h < HitWhere::NUM_HITWHERES ; h++)
   {
      if (HitWhereIsValid((HitWhere::where_t)h))
      {
         String name = "cpiDataCache" + String(HitWhereString((HitWhere::where_t)h));
         registerStatsMetric("rob_timer", core->getId(), name, &(thread->m_cpiDataCache[h]));
      }
   }

   thread->m_outstandingLongLatencyCycles = SubsecondTime::Zero();
   thread->m_outstandingLongLatencyInsns = SubsecondTime::Zero();
   thread->m_lastAccountedMemoryCycle = SubsecondTime::Zero();
   registerStatsMetric("rob_timer", core->getId(), "outstandingLongLatencyInsns", &thread->m_outstandingLongLatencyInsns);
   registerStatsMetric("rob_timer", core->getId(), "outstandingLongLatencyCycles", &thread->m_outstandingLongLatencyCycles);

   thread->m_loads_count = 0;
   thread->m_loads_latency = SubsecondTime::Zero();
   thread->m_stores_count = 0;
   thread->m_stores_latency = SubsecondTime::Zero();

   registerStatsMetric("rob_timer", core->getId(), "loads-count", &thread->m_loads_count);
   registerStatsMetric("rob_timer", core->getId(), "loads-latency", &thread->m_loads_latency);
   registerStatsMetric("rob_timer", core->getId(), "stores-count", &thread->m_stores_count);
   registerStatsMetric("rob_timer", core->getId(), "stores-latency", &thread->m_stores_latency);

   thread->m_totalProducerInsDistance = 0;
   thread->m_totalConsumers = 0;
   thread->m_producerInsDistance.resize(windowSize, 0);

   registerStatsMetric("rob_timer", core->getId(), "totalProducerInsDistance", &thread->m_totalProducerInsDistance);
   registerStatsMetric("rob_timer", core->getId(), "totalConsumers", &thread->m_totalConsumers);
   for (unsigned int i = 0; i < thread->m_producerInsDistance.size(); i++)
   {
      String name = "producerInsDistance[" + itostr(i) + "]";
      registerStatsMetric("rob_timer", core->getId(), name, &(thread->m_producerInsDistance[i]));
   }

   if (m_mlp_histogram)
   {
      thread->m_outstandingLoads.resize(HitWhere::NUM_HITWHERES);
      for (unsigned int h = HitWhere::WHERE_FIRST ; h < HitWhere::NUM_HITWHERES ; h++)
      {
         if (HitWhereIsValid((HitWhere::where_t)h))
         {
            thread->m_outstandingLoads[h].resize(MAX_OUTSTANDING, SubsecondTime::Zero());
            for(unsigned int i = 0; i < MAX_OUTSTANDING; ++i)
            {
               String name = String("outstandingLoads.") + HitWhereString((HitWhere::where_t)h) + "[" + itostr(i) + "]";
               registerStatsMetric("rob_timer", core->getId(), name, &(thread->m_outstandingLoads[h][i]));
            }
         }
      }

      thread->m_outstandingLoadsAll.resize(MAX_OUTSTANDING, SubsecondTime::Zero());
      for(unsigned int i = 0; i < MAX_OUTSTANDING; ++i)
      {
         String name = String("outstandingLoadsAll") + "[" + itostr(i) + "]";
         registerStatsMetric("rob_timer", core->getId(), name, &(thread->m_outstandingLoadsAll[i]));
      }
   }
}

void RobSmtTimer::RobEntry::init(DynamicMicroOp *_uop, UInt64 sequenceNumber)
{
   dispatched = SubsecondTime::MaxTime();
   ready = SubsecondTime::MaxTime();
   readyMax = SubsecondTime::Zero();
   addressReady = SubsecondTime::MaxTime();
   addressReadyMax = SubsecondTime::Zero();
   issued = SubsecondTime::MaxTime();
   done = SubsecondTime::MaxTime();

   uop = _uop;
   uop->setSequenceNumber(sequenceNumber);

   numInlineDependants = 0;
   vectorDependants = NULL;

   numAddressProducers = 0;
}

void RobSmtTimer::RobEntry::free()
{
   delete uop;
   if (vectorDependants)
      delete vectorDependants;
}

void RobSmtTimer::RobEntry::addDependant(RobSmtTimer::RobEntry* dep)
{
   if (numInlineDependants < MAX_INLINE_DEPENDANTS)
   {
      inlineDependants[numInlineDependants++] = dep;
   }
   else
   {
      if (vectorDependants == NULL)
      {
         vectorDependants = new std::vector<RobEntry*>();
      }
      vectorDependants->push_back(dep);
   }
}

uint64_t RobSmtTimer::RobEntry::getNumDependants() const
{
   return numInlineDependants + (vectorDependants ? vectorDependants->size() : 0);
}

RobSmtTimer::RobEntry* RobSmtTimer::RobEntry::getDependant(size_t idx) const
{
   if (idx < MAX_INLINE_DEPENDANTS)
   {
      LOG_ASSERT_ERROR(idx < numInlineDependants, "Invalid idx %d", idx);
      return inlineDependants[idx];
   }
   else
   {
      LOG_ASSERT_ERROR(idx - MAX_INLINE_DEPENDANTS < vectorDependants->size(), "Invalid idx %d", idx);
      return (*vectorDependants)[idx - MAX_INLINE_DEPENDANTS];
   }
}

void RobSmtTimer::setDependencies(smtthread_id_t thread_id, RobEntry *entry)
{
   RobThread *thread = m_rob_threads[thread_id];

   // Add = calculate dependencies, add yourself to list of depenants
   // If no dependants in window: set ready = now()
   uint64_t lowestValidSequenceNumber = thread->rob.size() > 0 ? thread->rob.front().uop->getSequenceNumber() : entry->uop->getSequenceNumber();

   if (entry->uop->getMicroOp()->isStore())
      setStoreAddressProducers(thread_id, entry, lowestValidSequenceNumber);

   thread->registerDependencies->setDependencies(*entry->uop, lowestValidSequenceNumber);
   thread->memoryDependencies->setDependencies(*entry->uop, lowestValidSequenceNumber);

   if (m_store_to_load_forwarding && entry->uop->getMicroOp()->isLoad())
   {
      for(unsigned int i = 0; i < entry->uop->getDependenciesLength(); ++i)
      {
         RobEntry *prodEntry = this->findEntryBySequenceNumber(thread_id, entry->uop->getDependency(i));
         // If we depend on a store
         if (prodEntry->uop->getMicroOp()->isStore())
         {
            // Remove dependency on the store (which won't execute until it reaches the front of the ROB)
            entry->uop->removeDependency(entry->uop->getDependency(i));

            // Add dependencies to the producers of the value being stored instead
            // Remark: one of these may be producing the store address, but because the store has to be
            //         disambiguated, it's correct to have the load depend on the address producers as well.
            for(unsigned int j = 0; j < prodEntry->uop->getDependenciesLength(); ++j)
               entry->uop->addDependency(prodEntry->uop->getDependency(j));

            break;
         }
      }
   }

   // Add ourselves to the dependants list of the uops we depend on
   uint64_t minProducerDistance = UINT64_MAX;
   thread->m_totalConsumers += 1 ;
   uint64_t deps_to_remove[8], num_dtr = 0;
   for(unsigned int i = 0; i < entry->uop->getDependenciesLength(); ++i)
   {
      UInt64 dependencySequenceNumber = entry->uop->getDependency(i);
      if (dependencySequenceNumber < lowestValidSequenceNumber)
      {
         // dependency is already done (this will be an intra-instruction dependency, register/memoryDependencies won't return anything < lowestValidSequenceNumber)
         deps_to_remove[num_dtr++] = entry->uop->getDependency(i);
      }
      else
      {
         RobEntry *prodEntry = this->findEntryBySequenceNumber(thread_id, entry->uop->getDependency(i));
         minProducerDistance = std::min( minProducerDistance,  entry->uop->getSequenceNumber() - prodEntry->uop->getSequenceNumber() );
         if (prodEntry->done != SubsecondTime::MaxTime())
         {
            // If producer is already done (but hasn't reached writeback stage), remove it from our dependency list
            deps_to_remove[num_dtr++] = entry->uop->getDependency(i);
            entry->readyMax = std::max(entry->readyMax, prodEntry->done);
         }
         else
         {
            prodEntry->addDependant(entry);
         }
      }
   }

   #ifdef DEBUG_PERCYCLE
   // Make sure we are in the dependant list of all of our address producers
   for(unsigned int i = 0; i < entry->getNumAddressProducers(); ++i)
   {
      if (thread->rob.size() && entry->getAddressProducer(i) >= thread->rob[0].uop->getSequenceNumber())
      {
         RobEntry *prodEntry = this->findEntryBySequenceNumber(thread_id, entry->getAddressProducer(i));
         bool found = false;
         for(unsigned int j = 0; j < prodEntry->getNumDependants(); ++j)
            if (prodEntry->getDependant(j) == entry)
            {
               found = true;
               break;
            }
         LOG_ASSERT_ERROR(found == true, "Store %ld depends on %ld for address production, but is not in its dependants list",
                          entry->uop->getSequenceNumber(), prodEntry->uop->getSequenceNumber());
      }
   }
   #endif

   if (minProducerDistance != UINT64_MAX)
   {
      thread->m_totalProducerInsDistance += minProducerDistance;
      // KENZO: not sure why the distance can be larger than the windowSize, but it happens...
      if (minProducerDistance >= thread->m_producerInsDistance.size())
         minProducerDistance = thread->m_producerInsDistance.size()-1;
      thread->m_producerInsDistance[ minProducerDistance ]++ ;
   }
   else
   {
      // Not depending on any instruction in the rob
      thread->m_producerInsDistance[ 0 ] += 1 ;
   }

   // If there are any dependencies to be removed, do this after iterating over them (don't mess with the list we're reading)
   LOG_ASSERT_ERROR(num_dtr < sizeof(deps_to_remove)/sizeof(8), "Have to remove more dependencies than I expected");
   for(uint64_t i = 0; i < num_dtr; ++i)
      entry->uop->removeDependency(deps_to_remove[i]);
   if (entry->uop->getDependenciesLength() == 0)
   {
      // We have no dependencies in the ROB: mark ourselves as ready
      entry->ready = entry->readyMax;
   }
}

void RobSmtTimer::setStoreAddressProducers(smtthread_id_t thread_id, RobEntry *entry, uint64_t lowestValidSequenceNumber)
{
   RobThread *thread = m_rob_threads[thread_id];

   for(unsigned int i = 0; i < entry->uop->getMicroOp()->getAddressRegistersLength(); ++i)
   {
      xed_reg_enum_t reg = entry->uop->getMicroOp()->getAddressRegister(i);
      uint64_t addressProducer = thread->registerDependencies->peekProducer(reg, lowestValidSequenceNumber);
      if (addressProducer != INVALID_SEQNR)
      {
         RobEntry *prodEntry = findEntryBySequenceNumber(thread_id, addressProducer);
         if (prodEntry->done != SubsecondTime::MaxTime())
            entry->addressReadyMax = std::max(entry->addressReadyMax, prodEntry->done);
         else
            entry->addAddressProducer(addressProducer);
      }
   }
   if (entry->getNumAddressProducers() == 0)
      entry->addressReady = entry->addressReadyMax;
}

RobSmtTimer::RobEntry *RobSmtTimer::findEntryBySequenceNumber(smtthread_id_t thread_id, UInt64 sequenceNumber)
{
   Rob &rob = m_rob_threads[thread_id]->rob;
   // Assumption: MicroOps in the ROB are numbered sequentially, none of them are removed halfway
   UInt64 first = rob[0].uop->getSequenceNumber();
   UInt64 position = sequenceNumber - first;
   LOG_ASSERT_ERROR(position < rob.size(), "Sequence number %ld outside of ROB", sequenceNumber);
   RobEntry *entry = &rob[position];
   LOG_ASSERT_ERROR(entry->uop->getSequenceNumber() == sequenceNumber, "Sequence number %ld unexpectedly not at ROB position %ld", sequenceNumber, position);
   return entry;
}

uint64_t RobSmtTimer::threadNumSurplusInstructions(smtthread_id_t thread_num)
{
   RobThread *thread = m_rob_threads[thread_num];
   return thread->rob.size() - thread->m_num_in_rob;
}

bool RobSmtTimer::threadHasEnoughInstructions(smtthread_id_t thread_num)
{
   return threadNumSurplusInstructions(thread_num) > 2*dispatchWidth;
}

void RobSmtTimer::notifyNumActiveThreadsChange()
{
   computeCurrentWindowsize();
}

void RobSmtTimer::pushInstructions(smtthread_id_t thread_id, const std::vector<DynamicMicroOp*>& insts)
{
   RobThread *thread = m_rob_threads[thread_id];

   // Collect new microops

   for (std::vector<DynamicMicroOp*>::const_iterator it = insts.begin(); it != insts.end(); it++ )
   {
      if ((*it)->isSquashed())
      {
         delete *it;
         continue;
      }

      RobEntry *entry = &thread->rob.next();
      entry->init(*it, thread->nextSequenceNumber++);

      #ifdef DEBUG_PERCYCLE
         std::cout<<"** ["<<int(thread_id)<<"] simulate: "<<entry->uop->getMicroOp()->toShortString(true)<<std::endl;
      #endif

      thread->m_uop_type_count[(*it)->getMicroOp()->getSubtype()]++;
      thread->m_uops_total++;
      if ((*it)->getMicroOp()->isX87()) thread->m_uops_x87++;
      if ((*it)->getMicroOp()->isPause()) thread->m_uops_pause++;

      if (thread->m_uops_total > 10000 && thread->m_uops_x87 > thread->m_uops_total / 20)
         LOG_PRINT_WARNING_ONCE("Significant fraction of x87 instructions encountered, accuracy will be low. Compile without -mno-sse2 -mno-sse3 to avoid.");
   }
}

boost::tuple<uint64_t,SubsecondTime> RobSmtTimer::returnLatency(smtthread_id_t thread_id)
{
   RobThread *thread = m_rob_threads[thread_id];

   // Return latency from previous execute() calls

   SubsecondTime returnLat = SubsecondTime::Zero();
   if (now > thread->now)
   {
      returnLat = now - thread->now;
      thread->now = now;
   }

   #ifdef DEBUG_PERCYCLE
      if (returnLat > SubsecondTime::Zero())
         std::cout<<"** ["<<int(thread_id)<<"] return "<<returnLat<<" latency cycles"<<std::endl<<std::endl<<std::endl;
   #endif

   // Calculate number of instructions executed for this thread since last call
   uint64_t instrs = thread->instrs - thread->instrs_returned;
   thread->instrs_returned = thread->instrs;

   return boost::tuple<uint64_t,SubsecondTime>(instrs, returnLat);
}

void RobSmtTimer::execute()
{
   while (true)
   {
     if (!canExecute())
        break;
     executeCycle();
   }
}

void RobSmtTimer::synchronize(smtthread_id_t thread_id, SubsecondTime time)
{
   // NOTE: depending on how far we jumped ahead (usually a considerable amount),
   //       we may want to flush the ROB and reset other queues
   #ifdef DEBUG_PERCYCLE
      std::cout<<"** ["<<int(thread_id)<<"] jump ahead after wake by "<<(time - m_rob_threads[thread_id]->now)<<" cycles"<<std::endl;
   #endif
   m_rob_threads[thread_id]->now = time;
   m_threads[thread_id]->in_wakeup = false;
   // Fast-forward core time to earliest thread time
   SubsecondTime earliest = SubsecondTime::MaxTime();
   for(smtthread_id_t thread_num = 0; thread_num < m_threads.size(); ++thread_num)
      if (m_rob_threads[thread_num]->now < earliest)
         earliest = m_rob_threads[thread_num]->now;
   if (earliest > now.getElapsedTime())
   {
      now.setElapsedTime(earliest);
      #ifdef DEBUG_PERCYCLE
         std::cout<<"** ["<<int(thread_id)<<"] fast-forwarding core time to "<<toCycles(now)<<" cycles"<<std::endl;
      #endif
   }
}

bool RobSmtTimer::tryDispatch(smtthread_id_t thread_num, SubsecondTime &next_event)
{
   // Dispatch up to dispatchWidth instructions, all from a single thread
   RobThread *thread = m_rob_threads[thread_num];
   uint32_t instrs_dispatched = 0, uops_dispatched = 0;

   // Unless we set a new stall condition later on, assume no stall occured
   thread->m_cpiCurrentFrontEndStall = NULL;

   while(thread->m_num_in_rob < currentWindowSize)
   {
      LOG_ASSERT_ERROR(thread->m_num_in_rob < thread->rob.size(), "Expected sufficient uops for dispatching in pre-ROB buffer, but didn't find them");

      RobEntry *entry = &thread->rob.at(thread->m_num_in_rob);
      DynamicMicroOp &uop = *entry->uop;

      // Dispatch up to 4 instructions
      if (uops_dispatched == dispatchWidth)
         break;

      // This is actually in the decode stage, there's a buffer between decode and dispatch
      // so we shouldn't do this here.
      //// First instruction can be any size, but second and subsequent ones may only be single-uop
      //// So, if this is not the first instruction, break if the first uop is not also the last
      //if (instrs_dispatched > 0 && !uop.isLast())
      //   break;

      bool iCacheMiss = (uop.getICacheHitWhere() != HitWhere::L1I);
      if (iCacheMiss)
      {
         if (thread->in_icache_miss)
         {
            // We just took the latency for this instruction, now dispatch it
            #ifdef DEBUG_PERCYCLE
               std::cout<<"-- icache return"<<std::endl;
            #endif
            thread->in_icache_miss = false;
         }
         else
         {
            #ifdef DEBUG_PERCYCLE
               std::cout<<"-- icache miss("<<uop.getICacheLatency()<<")"<<std::endl;
            #endif
            thread->frontend_stalled_until = now + uop.getICacheLatency();
            thread->in_icache_miss = true;
            // Don't dispatch this instruction yet
            thread->m_cpiCurrentFrontEndStall = &thread->m_cpiInstructionCache[uop.getICacheHitWhere()];
            break;
         }
      }

      if (m_rs_entries_used == rsEntries)
      {
         thread->m_cpiCurrentFrontEndStall = &thread->m_cpiRSFull;
         break;
      }

      setDependencies(thread_num, entry);

      entry->dispatched = now;
      ++thread->m_num_in_rob;
      ++m_rs_entries_used;

      uops_dispatched++;
      if (uop.isLast())
         instrs_dispatched++;

      // If uop is already ready, we may need to issue it in the following cycle
      entry->ready = std::max(entry->ready, (now + 1ul).getElapsedTime());
      next_event = std::min(next_event, entry->ready);

      thread->next_event = std::min(thread->next_event, next_event);

      #ifdef DEBUG_PERCYCLE
         std::cout<<"["<<int(thread_num)<<"] DISPATCH "<<uop.getMicroOp()->toShortString()<<std::endl;
      #endif

      #ifdef ASSERT_SKIP
         LOG_ASSERT_ERROR(will_skip == false, "Cycle would have been skipped but stuff happened");
      #endif

      // Mispredicted branch
      if (uop.getMicroOp()->isBranch() && uop.isBranchMispredicted())
      {
         thread->frontend_stalled_until = SubsecondTime::MaxTime();
         #ifdef DEBUG_PERCYCLE
            std::cout<<"-- branch mispredict"<<std::endl;
         #endif
         thread->m_cpiCurrentFrontEndStall = &thread->m_cpiBranchPredictor;
         break;
      }
   }

   if (thread->m_num_in_rob < currentWindowSize)
      next_event = std::min(thread->frontend_stalled_until, next_event);

   return uops_dispatched > 0;
}

SubsecondTime* RobSmtTimer::findCpiComponent(smtthread_id_t thread_num)
{
   RobThread *thread = m_rob_threads[thread_num];

   // Determine the CPI component corresponding to the first non-committed instruction
   for(uint64_t i = 0; i < thread->m_num_in_rob; ++i)
   {
      RobEntry *entry = &thread->rob.at(i);
      DynamicMicroOp *uop = entry->uop;
      // Skip over completed instructions
      if (entry->done < now)
         continue;
      // Nothing currently executing, CPI component will be Base or Branch/Icache as determined by front-end
      if (entry->issued >= now)
         return NULL;
      // This is the first instruction in the ROB which is still executing
      // Assume everyone is blocked on this one
      // Assign 100% of this cycle to this guy's CPI component
      if (uop->getMicroOp()->isSerializing() || uop->getMicroOp()->isMemBarrier())
         return &thread->m_cpiSerialization;
      else if (uop->getMicroOp()->isLoad() || uop->getMicroOp()->isStore())
         return &thread->m_cpiDataCache[uop->getDCacheHitWhere()];
      else
         return NULL;
   }
   // No instruction is currently executing
   return NULL;
}

SubsecondTime RobSmtTimer::doDispatch()
{
   SubsecondTime next_event = SubsecondTime::MaxTime();
   smtthread_id_t thread_first = dispatch_thread, thread_dispatched_from = dispatch_thread;
   bool hasDispatched = false;

   // Dispatch round-robin from threads that are not blocked due to I-cache miss, full ROB, idle, etc.

   smtthread_id_t thread_num = thread_first;
   do
   {
      SmtThread *smt_thread = m_threads[thread_num];
      RobThread *thread = m_rob_threads[thread_num];
      SubsecondTime *cpiComponent = NULL;
      SubsecondTime *cpiRobHead = findCpiComponent(thread_num);

      if (thread->frontend_stalled_until > now)
      {
         // front-end should not be stalled due to I-cache miss or branch misprediction
         assert(thread->m_cpiCurrentFrontEndStall);
         // canDispatch returned false, but not because the ROB was full: previous stall condition
         // (I-cache miss or BP-misprediction) still applies
         if (cpiRobHead)
            // Prioritize back-end stalls over front-end stalls
            cpiComponent = cpiRobHead;
         else
            cpiComponent = thread->m_cpiCurrentFrontEndStall;
      }
      else if (!smt_thread->running || thread->now > now)
      {
         // thread should not be idle, nor should it live way in the future because it just woke from idle
         cpiComponent = &thread->m_cpiIdle;
      }
      else if (thread->m_num_in_rob >= currentWindowSize)
      {
         // Could not dispatch because of a full ROB: determine which instruction we're stalling on
         cpiComponent = cpiRobHead ? cpiRobHead : &thread->m_cpiBase;
      }
      else
      {
         // Front-end is fine, lets try to dispatch new instructions

         if (hasDispatched)
         {
            // Could have dispatched something, but a previous thread has already dispatched
            cpiComponent = &thread->m_cpiSMT;
         }
         else
         {
            hasDispatched = tryDispatch(thread_num, next_event);
            if (hasDispatched)
               thread_dispatched_from = thread_num;

            if (thread->m_cpiCurrentFrontEndStall)
               // if tryDispatch failed, it will have set m_cpiCurrentFrontEndStall
               cpiComponent = thread->m_cpiCurrentFrontEndStall;
            else
               // if it did dispatch something, add to base CPI
               cpiComponent = &thread->m_cpiBase;
         }
      }

      LOG_ASSERT_ERROR(cpiComponent != NULL, "We expected cpiComponent to be set, but it wasn't");
      *cpiComponent += now.getPeriod();

      thread_num = (thread_num + 1) % m_threads.size();
   }
   while (thread_num != thread_first);

   // Either someone has issued, in the following cycle we'll start with the one following the thread that did issue
   // Or no-one issued, then thread_dispatched_from == thread_first, so we'll continue with the one following the previous first thread
   dispatch_thread = (thread_dispatched_from + 1) % m_threads.size();

   return next_event;
}

void RobSmtTimer::issueInstruction(smtthread_id_t thread_num, uint64_t idx, SubsecondTime &next_event)
{
   SmtThread *smt_thread = m_threads[thread_num];
   RobThread *thread = m_rob_threads[thread_num];
   RobEntry *entry = &thread->rob[idx];
   DynamicMicroOp &uop = *entry->uop;

   if ((uop.getMicroOp()->isLoad() || uop.getMicroOp()->isStore())
      && uop.getDCacheHitWhere() == HitWhere::UNKNOWN)
   {
      MemoryResult res = smt_thread->core->accessMemory(
         Core::NONE,
         uop.getMicroOp()->isLoad() ? Core::READ : Core::WRITE,
         uop.getAddress().address,
         NULL,
         uop.getMicroOp()->getMemoryAccessSize(),
         Core::MEM_MODELED_RETURN,
         uop.getMicroOp()->getInstruction() ? uop.getMicroOp()->getInstruction()->getAddress() : static_cast<uint64_t>(NULL),
         now.getElapsedTime()
      );
      uint64_t latency = SubsecondTime::divideRounded(res.latency, now.getPeriod());

      uop.setExecLatency(uop.getExecLatency() + latency); // execlatency already contains bypass latency
      uop.setDCacheHitWhere(res.hit_where);
   }

   if (uop.getMicroOp()->isLoad())
      load_queue.getCompletionTime(now, uop.getExecLatency() * now.getPeriod(), uop.getAddress().address);
   else if (uop.getMicroOp()->isStore())
      store_queue.getCompletionTime(now, uop.getExecLatency() * now.getPeriod(), uop.getAddress().address);

   ComponentTime cycle_depend = now + uop.getExecLatency();   // When result is available for dependent instructions
   SubsecondTime cycle_done = cycle_depend + 1ul;             // When the instruction can be committed

   if (uop.getMicroOp()->isLoad())
   {
      thread->m_loads_count++;
      thread->m_loads_latency += uop.getExecLatency() * now.getPeriod();
   }
   else if (uop.getMicroOp()->isStore())
   {
      thread->m_stores_count++;
      thread->m_stores_latency += uop.getExecLatency() * now.getPeriod();
   }

   if (uop.getMicroOp()->isStore())
   {
      last_store_done = std::max(last_store_done, cycle_done);
      cycle_depend = now + 1ul;                            // For stores, forward the result immediately
      // Stores can be removed from the ROB once they're issued to the memory hierarchy
      // Dependent operations such as SFENCE and synchronization instructions need to wait until last_store_done
      cycle_done = now + 1ul;

      LOG_ASSERT_ERROR(entry->addressReady <= entry->ready, "%ld: Store address cannot be ready (%ld) later than the whole uop is (%ld)",
                       entry->uop->getSequenceNumber(), entry->addressReady.getPS(), entry->ready.getPS());
   }

   if (m_rob_contention)
      m_rob_contention->doIssue(uop);

   entry->issued = now;
   entry->done = cycle_done;
   next_event = std::min(next_event, entry->done);

   --m_rs_entries_used;

   #ifdef DEBUG_PERCYCLE
      std::cout<<"["<<int(thread_num)<<"] ISSUE    "<<entry->uop->getMicroOp()->toShortString()<<"   latency="<<uop.getExecLatency()<<std::endl;
   #endif

   for(size_t idx = 0; idx < entry->getNumDependants(); ++idx)
   {
      RobEntry *depEntry = entry->getDependant(idx);
      LOG_ASSERT_ERROR(depEntry->uop->getDependenciesLength()> 0, "??");

      // Remove uop from dependency list and update readyMax
      depEntry->readyMax = std::max(depEntry->readyMax, cycle_depend.getElapsedTime());
      depEntry->uop->removeDependency(uop.getSequenceNumber());

      // If all dependencies are resolved, mark the uop ready
      if (depEntry->uop->getDependenciesLength() == 0)
      {
         depEntry->ready = depEntry->readyMax;
         //std::cout<<"    ready @ "<<depEntry->ready<<std::endl;
      }

      // For stores, check if their address has been produced
      if (depEntry->uop->getMicroOp()->isStore() && depEntry->addressReady == SubsecondTime::MaxTime())
      {
         bool ready = true;
         for(unsigned int i = 0; i < depEntry->getNumAddressProducers(); ++i)
         {
            uint64_t addressProducer = depEntry->getAddressProducer(i);
            RobEntry *prodEntry = addressProducer >= thread->rob.front().uop->getSequenceNumber()
                                ? this->findEntryBySequenceNumber(thread_num, addressProducer) : NULL;

            if (prodEntry == entry)
            {
               // The instruction we just executed is producing an address. Update the store's addressReadyMax
               depEntry->addressReadyMax = std::max(depEntry->addressReadyMax, cycle_depend.getElapsedTime());
            }

            if (prodEntry && prodEntry->done == SubsecondTime::MaxTime())
            {
               // An address producer has not yet been issued: address remains not ready
               ready = false;
            }
         }

         if (ready)
         {
            // We did not find any address producing instructions that have not yet been issued.
            // Store address will be ready at addressReadyMax
            depEntry->addressReady = depEntry->addressReadyMax;
         }
      }
   }

   // After issuing a mispredicted branch: allow the ROB to refill after flushing the pipeline
   if (uop.getMicroOp()->isBranch() && uop.isBranchMispredicted())
   {
      thread->frontend_stalled_until = now + (misprediction_penalty - 2); // The frontend needs to start 2 cycles earlier to get a total penalty of <misprediction_penalty>
      #ifdef DEBUG_PERCYCLE
         std::cout<<"-- branch resolve"<<std::endl;
      #endif
   }
}

bool RobSmtTimer::tryIssue(smtthread_id_t thread_num, SubsecondTime &next_event, bool would_have_skipped)
{
   uint64_t num_issued = 0;
   bool head_of_queue = true, no_more_load = false, no_more_store = false, have_unresolved_store = false;
   RobThread *thread = m_rob_threads[thread_num];
   SubsecondTime thread_next_event = SubsecondTime::MaxTime();

   for(uint64_t i = 0; i < thread->m_num_in_rob; ++i)
   {
      RobEntry *entry = &thread->rob.at(i);
      DynamicMicroOp *uop = entry->uop;


      if (entry->done != SubsecondTime::MaxTime())
      {
         thread_next_event = std::min(thread_next_event, entry->done);
         continue;                     // already done
      }

      thread_next_event = std::min(thread_next_event, entry->ready);


      // See if we can issue this instruction

      bool canIssue = false;

      if (entry->ready > now)
         canIssue = false;          // blocked by dependency

      else if ((no_more_load && uop->getMicroOp()->isLoad()) || (no_more_store && uop->getMicroOp()->isStore()))
         canIssue = false;          // blocked by mfence

      else if (uop->getMicroOp()->isSerializing())
      {
         if (head_of_queue && last_store_done <= now)
            canIssue = true;
         else
            break;
      }

      else if (uop->getMicroOp()->isMemBarrier())
      {
         if (head_of_queue && last_store_done <= now)
            canIssue = true;
         else
            // Don't issue any memory operations following a memory barrier
            no_more_load = no_more_store = true;
            // FIXME: L/SFENCE
      }

      else if (!m_rob_contention && num_issued == dispatchWidth)
         canIssue = false;          // no issue contention: issue width == dispatch width

      else if (uop->getMicroOp()->isLoad() && !load_queue.hasFreeSlot(now))
         canIssue = false;          // load queue full

      else if (uop->getMicroOp()->isLoad() && m_no_address_disambiguation && have_unresolved_store)
         canIssue = false;          // preceding store with unknown address

      else if (uop->getMicroOp()->isStore() && (!head_of_queue || !store_queue.hasFreeSlot(now)))
         canIssue = false;          // store queue full

      else
         canIssue = true;           // issue!


      // canIssue already marks issue ports as in use, so do this one last
      if (canIssue && m_rob_contention && ! m_rob_contention->tryIssue(*uop))
         canIssue = false;          // blocked by structural hazard


      if (canIssue)
      {
         num_issued++;
         issueInstruction(thread_num, i, thread_next_event);


         // Calculate memory-level parallelism (MLP) for long-latency loads (but ignore overlapped misses)
         if (uop->getMicroOp()->isLoad() && uop->isLongLatencyLoad() && uop->getDCacheHitWhere() != HitWhere::L1_OWN)
         {
            if (thread->m_lastAccountedMemoryCycle < now) thread->m_lastAccountedMemoryCycle = now;

            SubsecondTime done = std::max( now.getElapsedTime(), entry->done );
            // Ins will be outstanding for until it is done. By account beforehand I don't need to
            // worry about fast-forwarding simulations
            thread->m_outstandingLongLatencyInsns += (done - now);

            // Only account for the cycles that have not yet been accounted for by other long
            // latency misses (don't account cycles twice).
            if ( done > thread->m_lastAccountedMemoryCycle )
            {
               thread->m_outstandingLongLatencyCycles += done - thread->m_lastAccountedMemoryCycle;
               thread->m_lastAccountedMemoryCycle = done;
            }

            #ifdef ASSERT_SKIP
            LOG_ASSERT_ERROR( thread->m_outstandingLongLatencyInsns >= thread->m_outstandingLongLatencyCycles, "MLP calculation is wrong: MLP cannot be < 1!"  );
            #endif
         }


         #ifdef ASSERT_SKIP
            LOG_ASSERT_ERROR(will_skip == false, "Cycle would have been skipped but stuff happened");
            LOG_ASSERT_ERROR(would_have_skipped == false, "Cycle would have been skipped but stuff happened");
         #endif
      }
      else
      {
         head_of_queue = false;     // Subsequent instructions are not at the head of the ROB

         if (uop->getMicroOp()->isStore() && entry->addressReady > now)
            have_unresolved_store = true;

         if (inorder)
            // In-order: only issue from head of the ROB
            break;
      }


      if (m_rob_contention)
      {
         if (m_rob_contention->noMore())
            break;
      }
      else
      {
         if (num_issued == dispatchWidth)
            break;
      }
   }

   thread->next_event = thread_next_event;
   next_event = std::min(next_event, thread_next_event);

   return num_issued > 0;
}

SubsecondTime RobSmtTimer::doIssue()
{
   SubsecondTime next_event = SubsecondTime::MaxTime();
   smtthread_id_t thread_first = issue_thread;

   if (m_rob_contention)
      m_rob_contention->initCycle(now);

   // Issue round-robin, from a single-thread only (interleaved multithreading, SMT would need to check all ROBs)

   smtthread_id_t thread_num = thread_first;
   do
   {
      RobThread *thread = m_rob_threads[thread_num];

      if (thread->next_event > now)
      {
         #ifdef ASSERT_SKIP
            tryIssue(thread_num, next_event, true);
         #endif
      }
      else if (m_rob_threads[thread_num]->m_num_in_rob > 0)
      {
         bool hasIssued = tryIssue(thread_num, next_event);
         if (hasIssued && !simultaneousIssue)
            break;
      }
      thread_num = (thread_num + 1) % m_threads.size();
   } while (thread_num != thread_first);

   // Either someone has issued, in the following cycle we'll start with the one following the thread that did issue
   // Or no-one issued, then thread_num == thread_first, so we'll continue with the one following the previous first thread
   issue_thread = (thread_num + 1) % m_threads.size();

   return next_event;
}

SubsecondTime RobSmtTimer::doCommit()
{
   // Commit all done instructions from all threads
   for(smtthread_id_t thread_num = 0; thread_num < m_threads.size(); ++thread_num)
   {
      uint64_t num_committed = 0;
      RobThread *thread = m_rob_threads[thread_num];

      while(thread->rob.size() && (thread->rob.front().done <= now))
      {
         RobEntry *entry = &thread->rob.front();

         #ifdef DEBUG_PERCYCLE
            std::cout<<"["<<int(thread_num)<<"] COMMIT   "<<entry->uop->getMicroOp()->toShortString()<<std::endl;
         #endif

         InstructionTracer::uop_times_t times = {
            entry->dispatched,
            entry->issued,
            entry->done,
            now
         };
         thread->core->getPerformanceModel()->traceInstruction(entry->uop, &times);

         if (entry->uop->isLast())
            thread->instrs++;

         entry->free();
         thread->rob.pop();
         thread->m_num_in_rob--;

         #ifdef ASSERT_SKIP
            LOG_ASSERT_ERROR(will_skip == false, "Cycle would have been skipped but stuff happened");
         #endif

         ++num_committed;
         if (num_committed == commitWidth)
            break;
      }
   }

   if (m_rob_threads[0]->rob.size()) // NOTE: for SMT, skip is ignored anyway
      return m_rob_threads[0]->rob.front().done;
   else
      return SubsecondTime::MaxTime();
}

bool RobSmtTimer::canExecute(smtthread_id_t thread_num)
{
   SmtThread *smt_thread = m_threads[thread_num];
   RobThread *thread = m_rob_threads[thread_num];

   // Will this thread potentially dispatch this cycle?
   if (thread->frontend_stalled_until > now     // front-end is stalled due to I-cache miss or branch misprediction
       || thread->now > now                     // core lives way in the future because it just woke from idle
       || thread->m_num_in_rob >= currentWindowSize // ROB is full
       || !smt_thread->running                       // thread is not running
   )
   {
      return false;
   }
   else
   {
      return true;
   }
}

bool RobSmtTimer::canExecute()
{
   // Make sure we have sufficient instructions for each thread, else, do some more functional simulation
   for(smtthread_id_t thread_num = 0; thread_num < m_threads.size(); ++thread_num)
   {
      RobThread *thread = m_rob_threads[thread_num];

      if (!canExecute(thread_num))
      {
         // Thread will not dispatch, no need to consider it
         continue;
      }
      else if (thread->rob.size() < thread->m_num_in_rob + 2*dispatchWidth)
      {
         // We don't have enough instructions to dispatch <dispatchWidth> new ones. Ask for more before doing anything this cycle.
         return false;
      }
   }
   return true;
}

SubsecondTime RobSmtTimer::executeCycle()
{
   SubsecondTime latency = SubsecondTime::Zero();

   #ifdef DEBUG_PERCYCLE
      std::cout<<std::endl<<"###################################################################################"<<std::endl<<std::endl;
      std::cout<<"Running cycle "<<now<<std::endl;
   #endif

   // Model dispatch, issue and commit stages
   // Decode stage is not modeled, assumes the decoders can keep up with (up to) dispatchWidth uops per cycle

   SubsecondTime next_dispatch = doDispatch();
   SubsecondTime next_issue    = doIssue();
   SubsecondTime next_commit   = doCommit();


   #ifdef DEBUG_PERCYCLE
      #ifdef ASSERT_SKIP
         if (! will_skip)
         {
      #endif
         printRob();
      #ifdef ASSERT_SKIP
         }
      #endif
   #endif


   #ifdef DEBUG_PERCYCLE
      std::cout<<"Next event: D("<<next_dispatch<<") I("<<next_issue<<") C("<<next_commit<<")"<<std::endl;
   #endif
   SubsecondTime next_event = std::min(next_dispatch, std::min(next_issue, next_commit));
   SubsecondTime skip;
   if (m_threads.size() > 1)
   {
      // SMT: we would need to check all ROBs at all cycles. For now, don't bother skipping
      will_skip = false;
      skip = now.getPeriod();
      // Also, CPI stack code is concentrated at dispatch and wouldn't know about skipping
   }
   else if (next_event != SubsecondTime::MaxTime() && next_event > now + 1ul)
   {
      #ifdef DEBUG_PERCYCLE
         std::cout<<"++ Skip "<<(next_event - now)<<std::endl;
      #endif
      will_skip = true;
      skip = next_event - now;
   }
   else
   {
      will_skip = false;
      skip = now.getPeriod();
   }

   #ifdef ASSERT_SKIP
      now += now.getPeriod();
      latency += now.getPeriod();
      if (will_skip)
         time_skipped += now.getPeriod();
   #else
      now += skip;
      latency += skip;
      if (skip > now.getPeriod())
         time_skipped += (skip - now.getPeriod());
   #endif

   if (m_mlp_histogram)
   {
      for(smtthread_id_t thread_num = 0; thread_num < m_threads.size(); ++thread_num)
         countOutstandingMemop(thread_num, skip);
   }

   return latency;
}

void RobSmtTimer::countOutstandingMemop(smtthread_id_t thread_num, SubsecondTime time)
{
   RobThread *thread = m_rob_threads[thread_num];
   UInt64 counts[HitWhere::NUM_HITWHERES] = {0}, total = 0;

   for(unsigned int i = 0; i < thread->m_num_in_rob; ++i)
   {
      RobEntry *e = &thread->rob.at(i);
      if (e->done != SubsecondTime::MaxTime() && e->done > now && e->uop->getMicroOp()->isLoad())
      {
         ++counts[e->uop->getDCacheHitWhere()];
         ++total;
      }
   }

   for(unsigned int h = 0; h < HitWhere::NUM_HITWHERES; ++h)
      if (counts[h] > 0)
         thread->m_outstandingLoads[h][counts[h] >= MAX_OUTSTANDING ? MAX_OUTSTANDING-1 : counts[h]] += time;
   if (total > 0)
      thread->m_outstandingLoadsAll[total >= MAX_OUTSTANDING ? MAX_OUTSTANDING-1 : total] += time;
}

void RobSmtTimer::printRob(smtthread_id_t thread_num)
{
   RobThread *thread = m_rob_threads[thread_num];

   #ifdef DEBUG_PERCYCLE
      SmtThread *smt_thread = m_threads[thread_num];
      std::cout<<"** State:";
      if (smt_thread->in_wakeup)     std::cout<<" IN_WAKEUP";
      if (smt_thread->running)       std::cout<<" RUNNING";
      if (smt_thread->in_barrier)    std::cout<<" BARRIER";
      std::cout<<std::endl;
   #endif

   std::cout<<"** ROB state size("<<thread->m_num_in_rob<<") total("<<thread->rob.size()<<")";
   if (thread->frontend_stalled_until == SubsecondTime::MaxTime())
      std::cout<<" stalled(---)";
   else if (thread->frontend_stalled_until > now)
      std::cout<<" stalled("<<(thread->frontend_stalled_until - now)<<")";
   if (thread->in_icache_miss)
      std::cout << ", in I-cache miss";
   std::cout<<std::endl;

   if (thread->next_event > now)
      std::cout<<"** Next event: "<<(thread->next_event - now)<<std::endl;

   for(unsigned int i = 0; i < std::min(thread->m_num_in_rob + 16, (uint64_t)thread->rob.size()); ++i)
   {
      std::cout<<"   ["<<std::setw(3)<<i<<"]  ";
      RobEntry *e = &thread->rob.at(i);

      std::ostringstream state;
      if (i >= thread->m_num_in_rob) state<<"PREROB ";
      else if (e->done != SubsecondTime::MaxTime()) state<<"DONE@+"<<(e->done-now)<<"  ";
      else if (e->ready != SubsecondTime::MaxTime()) state<<"READY@+"<<(e->ready-now)<<"  ";
      else
      {
         state<<"DEPS ";
         for(uint32_t j = 0; j < e->uop->getDependenciesLength(); j++)
            state << std::dec << e->uop->getDependency(j) << " ";
      }
      std::cout<<std::left<<std::setw(20)<<state.str()<<"   ";
      std::cout<<std::right<<std::setw(10)<<e->uop->getSequenceNumber()<<"  ";
      if (e->uop->getMicroOp()->isLoad())
         std::cout<<"LOAD      ";
      else if (e->uop->getMicroOp()->isStore())
         std::cout<<"STORE     ";
      else
         std::cout<<"EXEC ("<<std::right<<std::setw(2)<<e->uop->getExecLatency()<<") ";
      if (e->uop->getMicroOp()->getInstruction())
      {
         std::cout<<std::hex<<e->uop->getMicroOp()->getInstruction()->getAddress()<<std::dec<<": "
                  <<e->uop->getMicroOp()->getInstruction()->getDisassembly();
         if (e->uop->getMicroOp()->isLoad() || e->uop->getMicroOp()->isStore())
            std::cout<<"  {0x"<<std::hex<<e->uop->getAddress().address<<std::dec<<"}";
      }
      else
         std::cout<<"(dynamic)";
      std::cout<<std::endl;
   }
   if (thread->rob.size() > thread->m_num_in_rob + 16)
      std::cout<<"   -----  "<<(thread->rob.size() - (thread->m_num_in_rob + 16))<<" more PREROB -----"<<std::endl;
}

void RobSmtTimer::printRob()
{
   std::cout<<"** ROB state @ "<<now<<std::endl;
   std::cout<<"   RS entries: "<<m_rs_entries_used<<std::endl;
   std::cout<<"   Outstanding loads: "<<load_queue.getNumUsed(now)<<"  stores: "<<store_queue.getNumUsed(now)<<std::endl;
   for(smtthread_id_t thread_num = 0; thread_num < m_threads.size(); ++thread_num)
   {
      if (m_threads.size() > 1)
         std::cout<<std::endl<<"* ROB thread "<<int(thread_num)<<std::endl;
      printRob(thread_num);
   }
}
