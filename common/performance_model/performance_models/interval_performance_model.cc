#include "core.h"
#include "log.h"
#include "interval_performance_model.h"
#include "branch_predictor.h"
#include "stats.h"
#include "lll_info.h"
#include "dvfs_manager.h"
#include "subsecond_time.h"
#include "micro_op.h"

#include <cstdio>

MicroOp* IntervalPerformanceModel::m_serialize_uop = NULL;
MicroOp* IntervalPerformanceModel::m_mfence_uop = NULL;

IntervalPerformanceModel::IntervalPerformanceModel(Core *core, int misprediction_penalty)
    : PerformanceModel(core)
    , interval_timer(core,
       this,
       misprediction_penalty,
       Sim()->getCfg()->getInt("perf_model/core/interval_timer/dispatch_width", 4),
       Sim()->getCfg()->getInt("perf_model/core/interval_timer/window_size", 96),
       Sim()->getCfg()->getBool("perf_model/core/interval_timer/fu_contention", false)
      )
    , m_state_uops_done(false)
    , m_state_icache_done(false)
    , m_state_num_reads_done(0)
    , m_state_num_writes_done(0)
    , m_state_num_nonmem_done(0)
    , m_state_insn_period(ComponentPeriod::fromFreqHz(1)) // ComponentPeriod() is private, this is a placeholder.  Will be updated at resetState()
    , m_instruction_count(0)
    , m_elapsed_time(Sim()->getDvfsManager()->getCoreDomain(core->getId()))
    , m_idle_elapsed_time(Sim()->getDvfsManager()->getCoreDomain(core->getId()))
    , m_dyninsn_count(0)
    , m_dyninsn_cost(0)
    , m_dyninsn_zero_count(0)
    , m_misalign_info(core)
{
   registerStatsMetric("performance_model", core->getId(), "instruction_count", &m_instruction_count);
   registerStatsMetric("performance_model", core->getId(), "elapsed_time", &m_elapsed_time);
   registerStatsMetric("performance_model", core->getId(), "idle_elapsed_time", &m_idle_elapsed_time);
   registerStatsMetric("performance_model", core->getId(), "dyninsn_count", &m_dyninsn_count);
   registerStatsMetric("performance_model", core->getId(), "dyninsn_cost", &m_dyninsn_cost);
   registerStatsMetric("performance_model", core->getId(), "dyninsn_zero_count", &m_dyninsn_zero_count);
#if DEBUG_DYN_INSN_LOG
   String filename;
   filename = "sim.dyninsn_log." + itostr(core->getId());
   filename = Sim()->getConfig()->formatOutputFileName(filename);
   m_dyninsn_log = std::fopen(filename.c_str(), "w");
#endif
#if DEBUG_INSN_LOG
   String insn_filename;
   insn_filename = "sim.insn_log." + itostr(core->getId());
   insn_filename = Sim()->getConfig()->formatOutputFileName(insn_filename);
   m_insn_log = std::fopen(insn_filename.c_str(), "w");
#endif
#if DEBUG_CYCLE_COUNT_LOG
   String cycle_filename;
   cycle_filename = "sim.cycle_log." + itostr(core->getId());
   cycle_filename = Sim()->getConfig()->formatOutputFileName(cycle_filename);
   m_cycle_log = std::fopen(cycle_filename.c_str(), "w");
#endif

   // Create a bitmask for filtering the L1 cache access addresses
   // The core model needs to handle this because the cache model cannot
   // Here, line size is assumed to be a power of 2
   UInt32 l1_line_size = Sim()->getCfg()->getInt("perf_model/l1_dcache/cache_block_size", 64);
   m_l1_line_mask = ~(l1_line_size - 1);

   m_cpiSyncFutex = SubsecondTime::Zero();
   m_cpiSyncPthreadMutex = SubsecondTime::Zero();
   m_cpiSyncPthreadCond = SubsecondTime::Zero();
   m_cpiSyncPthreadBarrier = SubsecondTime::Zero();
   m_cpiSyncJoin = SubsecondTime::Zero();
   m_cpiSyncDvfsTransition = SubsecondTime::Zero();
   registerStatsMetric("performance_model", core->getId(), "cpiSyncFutex", &m_cpiSyncFutex);
   registerStatsMetric("performance_model", core->getId(), "cpiSyncPthreadMutex", &m_cpiSyncPthreadMutex);
   registerStatsMetric("performance_model", core->getId(), "cpiSyncPthreadCond", &m_cpiSyncPthreadCond);
   registerStatsMetric("performance_model", core->getId(), "cpiSyncPthreadBarrier", &m_cpiSyncPthreadBarrier);
   registerStatsMetric("performance_model", core->getId(), "cpiSyncJoin", &m_cpiSyncJoin);
   registerStatsMetric("performance_model", core->getId(), "cpiSyncDvfsTransition", &m_cpiSyncDvfsTransition);

   m_cpiRecv = SubsecondTime::Zero();
   registerStatsMetric("performance_model", core->getId(), "cpiRecv", &m_cpiRecv);

   m_cpiITLBMiss = SubsecondTime::Zero();
   m_cpiDTLBMiss = SubsecondTime::Zero();
   registerStatsMetric("performance_model", core->getId(), "cpiITLBMiss", &m_cpiITLBMiss);
   registerStatsMetric("performance_model", core->getId(), "cpiDTLBMiss", &m_cpiDTLBMiss);

   m_cpiMemAccess = SubsecondTime::Zero();
   registerStatsMetric("performance_model", core->getId(), "cpiSyncMemAccess", &m_cpiMemAccess);

   m_cpiStartTime = SubsecondTime::Zero();
   m_cpiFastforwardTime = SubsecondTime::Zero();
   registerStatsMetric("performance_model", core->getId(), "cpiStartTime", &m_cpiStartTime);
   registerStatsMetric("performance_model", core->getId(), "cpiFastforwardTime", &m_cpiFastforwardTime);

   if (! m_serialize_uop) {
      m_serialize_uop = new MicroOp();
      UInt64 interval_sync_cost = 1;
      m_serialize_uop->makeExecute(0,0,XED_ICLASS_INVALID,"DynamicInsn-Serialize",interval_sync_cost,0,0);
      m_serialize_uop->setSerializing(true);
      m_serialize_uop->setFirst(true);
      m_serialize_uop->setLast(true);
   }

   if (! m_mfence_uop) {
      m_mfence_uop = new MicroOp();
      m_mfence_uop->makeExecute(0,0,XED_ICLASS_INVALID,"DynamicInsn-MFENCE",1,0,0);
      m_mfence_uop->setMemBarrier(true);
      m_mfence_uop->setFirst(true);
      m_mfence_uop->setLast(true);
   }

   resetState(); // Needed to clear the period value
}

IntervalPerformanceModel::~IntervalPerformanceModel()
{
#if DEBUG_DYN_INSN_LOG
   std::fclose(m_dyninsn_log);
#endif
#if DEBUG_INSN_LOG
   std::fclose(m_insn_log);
#endif
#if DEBUG_CYCLE_COUNT_LOG
   std::fclose(m_cycle_log);
#endif
}

void IntervalPerformanceModel::outputSummary(std::ostream &os) const
{
   os << "  Instructions: " << getInstructionCount() << std::endl
      << "  Cycles: " << m_elapsed_time.getCycleCount() << std::endl
      << "  Time: " << m_elapsed_time.getElapsedTime().getNS() << std::endl;

   if (getConstBranchPredictor())
      getConstBranchPredictor()->outputSummary(os);
}

bool IntervalPerformanceModel::handleInstruction(Instruction const* instruction)
{
   if (m_state_instruction == NULL)
   {
      m_state_instruction = instruction;
   }
   LOG_ASSERT_ERROR(m_state_instruction == instruction, "Error: The instruction has changed, but the internal state has not been reset!");

   // Get the period (current CPU frequency) for this instruction
   // Keep it constant during it's execution
   if (m_state_insn_period.getPeriod() == SubsecondTime::Zero())
   {
      m_state_insn_period = *(const_cast<ComponentPeriod*>(static_cast<const ComponentPeriod*>(m_elapsed_time)));
   }

   // Keep our current state of what has already been processed
   // Each getDynamicInstructionInfo() might cause an exception, but
   // we need to be sure to save what we have already computed

   if (!m_state_uops_done)
   {
      for(std::vector<const MicroOp*>::const_iterator it = instruction->getMicroOps().begin(); it != instruction->getMicroOps().end(); it++)
      {
         m_current_uops.push_back(*(*it));
         m_current_uops.back().setPeriod(m_state_insn_period);
      }
      m_state_uops_done = true;
   }

   // Find some information
   size_t num_loads = 0;
   size_t num_stores = 0;
   size_t exec_base_index = SIZE_MAX;
   // Find the first load
   size_t load_base_index = SIZE_MAX;
   // Find the first store
   size_t store_base_index = SIZE_MAX;
   for (size_t m = 0 ; m < m_current_uops.size() ; m++ )
   {
      if (m_current_uops[m].isExecute())
      {
         exec_base_index = m;
         break;
      }
      if (m_current_uops[m].isStore())
      {
         ++num_stores;
         if (store_base_index == SIZE_MAX)
            store_base_index = m;
      }
      if (m_current_uops[m].isLoad())
      {
         ++num_loads;
         if (load_base_index == SIZE_MAX)
            load_base_index = m;
      }
   }

   // Compute the iCache cost, and add to our cycle time
   if (!m_state_icache_done && Sim()->getConfig()->getEnableICacheModeling())
   {
      SubsecondTime icache_access_time = SubsecondTime::Zero();
      // Sometimes, these aren't real instructions (INST_SPAWN, etc), and therefore, we need to skip these
      if (instruction->getAddress() && !instruction->isDynamic() && m_current_uops.size() > 0 )
      {
         MemoryResult memres = getCore()->readInstructionMemory(instruction->getAddress(), sizeof(IntPtr));

         // For the interval model, for now, use integers for the cycle latencies
         UInt64 memory_cycle_latency = SubsecondTime::divideRounded(memres.latency, m_state_insn_period);

         // Set the hit_where information for the icache
         // The interval model will only add icache latencies if there hasn't been a hit.
         m_current_uops[0].setICacheHitWhere(memres.hit_where);
         m_current_uops[0].setICacheLatency(memory_cycle_latency);
      }
      m_state_icache_done = true;
   }

   // Graphite instruction operands
   const OperandList &ops = instruction->getOperands();

   // If we haven't gotten all of our read or write data yet, iterate over the operands
   for (size_t i = 0 ; i < ops.size() ; ++i)
   {
      const Operand &o = ops[i];

      if (o.m_type == Operand::MEMORY)
      {
         // For each memory operand, there exists a dynamic instruction to process
         DynamicInstructionInfo *info = getDynamicInstructionInfo(*instruction);
         if (!info)
            return false;

         // Because the interval model is currently in cycles, convert the data to cycles here before using it
         // Force the latencies into cycles for use in the original interval model
         // FIXME Update the Interval Timer to use SubsecondTime
         UInt64 memory_cycle_latency = SubsecondTime::divideRounded(info->memory_info.latency, m_state_insn_period);

         if (o.m_direction == Operand::READ)
         {

            // Operand::READ

            if (load_base_index != SIZE_MAX)
            {

               size_t load_index = load_base_index + m_state_num_reads_done;

               LOG_ASSERT_ERROR(info->type == DynamicInstructionInfo::MEMORY_READ,
                                "Expected memory read info, got: %d.", info->type);
               LOG_ASSERT_ERROR(load_index < m_current_uops.size(),
                                "Expected load_index(%x) to be less than uops.size()(%d).", load_index, m_current_uops.size());
               LOG_ASSERT_ERROR(m_current_uops[load_index].isLoad(),
                                "Expected uop %d to be a load.", load_index);

               // Update this uop with load latencies
               m_current_uops[load_index].setExecLatency(memory_cycle_latency);
               Memory::Access addr;
               addr.set(info->memory_info.addr & m_l1_line_mask); // Enforce L1 cache line dependencies here, because the cache models do not handle it
               m_current_uops[load_index].setAddress(addr);
               m_current_uops[load_index].setDCacheHitWhere(info->memory_info.hit_where);
               ++m_state_num_reads_done;
            }
            else
            {
               // Because of the differences between the PIN disassembler and disasm64, we sometimes don't always
               // have a perfect alignment between the number of load operations that exist on an instruction
               ++m_misalign_info.read_misses;
               m_misalign_info.read_total_miss_latency += memory_cycle_latency;
            }

         }
         else
         {
            // Operand::WRITE

            if (store_base_index != SIZE_MAX)
            {

               size_t store_index = store_base_index + m_state_num_writes_done;

               LOG_ASSERT_ERROR(info->type == DynamicInstructionInfo::MEMORY_WRITE,
                                "Expected memory write info, got: %d.", info->type);
               LOG_ASSERT_ERROR(store_index < m_current_uops.size(),
                                "Expected store_index(%d) to be less than uops.size()(%d).", store_index, m_current_uops.size());
               LOG_ASSERT_ERROR(m_current_uops[store_index].isStore(),
                                "Expected uop %d to be a store. [%d|%s]", store_index, m_current_uops[store_index].getType(), m_current_uops[store_index].toString().c_str());

               // Update this uop with store latencies.
               m_current_uops[store_index].setExecLatency(memory_cycle_latency);
               Memory::Access addr;
               addr.set(info->memory_info.addr & m_l1_line_mask); // Enforce L1 cache line dependencies because the cache model does not handle it
               m_current_uops[store_index].setAddress(addr);
               m_current_uops[store_index].setDCacheHitWhere(info->memory_info.hit_where);
               ++m_state_num_writes_done;
            }
            else
            {
               ++m_misalign_info.write_misses;
               m_misalign_info.write_total_miss_latency += memory_cycle_latency;
            }

         }

         // When we have finally finished processing this dynamic instruction, remove it from the queue
         popDynamicInstructionInfo();
      }
      else
      {
         ++m_state_num_nonmem_done;
      }

   }

   // Instruction cost resolution
   // Because getCost may fail if there are missing DynInstrInfo's, do not call getCost() anywhere else but here
   // If it fails, keep state because we are waiting for processing that needs to occur,
   // but some costs (I-cache access) have already been resolved
   SubsecondTime insn_cost = instruction->getCost(getCore());
   if (insn_cost == PerformanceModel::DyninsninfoNotAvailable())
      return false;

#if DEBUG_INSN_LOG
   if (insn_cost > 17)
   {
      fprintf(m_insn_log, "[%llu] ", (long long unsigned int)m_cycle_count);
      if (load_base_index != SIZE_MAX) {
         fprintf(m_insn_log, "L");
      }
      if (store_base_index != SIZE_MAX) {
         fprintf(m_insn_log, "S");
      }
      if (exec_base_index != SIZE_MAX) {
         fprintf(m_insn_log, "X");
#ifdef ENABLE_MICROOP_STRINGS
         fprintf(m_insn_log, "-%s:%s", instruction->getDisassembly().c_str(), instruction->getTypeName().c_str());
         fflush(m_insn_log);
#endif
      }
      fprintf(m_insn_log, "approx cost = %llu\n", (long long unsigned int)insn_cost);
   }
#endif

   // Determine branch miss/hit status for the interval model
   bool branch_mispredict;
   bool is_branch = (instruction->getType() == INST_BRANCH);

   if ( is_branch & (insn_cost != 1 * static_cast<SubsecondTime>(m_state_insn_period)) )
   {
      branch_mispredict = 1;
   }
   else
   {
      branch_mispredict = 0;
   }

   // Set whether the branch was mispredicted or not
   if (is_branch)
   {
      LOG_ASSERT_ERROR(m_current_uops[exec_base_index].isBranch(), "Expected to find a branch here.")
      m_current_uops[exec_base_index].setBranchMispredicted(branch_mispredict);
      // Do not update the execution latency of a branch instruction
      // The interval model will calculate the branch latency
   }

   // Do not update the cost of the exec unit, as we want to use the values that are there (unless we are a branch)

   // Insert an instruction into the interval model to indicate that time has passed
   uint32_t new_num_insns = 0;
   ComponentTime new_latency(m_elapsed_time.getLatencyGenerator()); // Get a new, empty holder for latency
   if(m_current_uops.size() > 0)
   {
      uint64_t new_latency_cycles;
      boost::tie(new_num_insns, new_latency_cycles) = simulate(m_current_uops);
      new_latency.addCycleLatency(new_latency_cycles);

#if DEBUG_INSN_LOG > 1
      fprintf(m_insn_log, "[%llu] ", (long long unsigned int)m_cycle_count);
      if (load_base_index != SIZE_MAX) {
         fprintf(m_insn_log, "L");
      }
      if (store_base_index != SIZE_MAX) {
         fprintf(m_insn_log, "S");
      }
      if (exec_base_index != SIZE_MAX) {
         fprintf(m_insn_log, "X");
#ifdef ENABLE_MICROOP_STRINGS
         fprintf(m_insn_log, "-%s", m_current_uops[exec_base_index].getInstructionOpcodeName().c_str());
#endif
      }
      fprintf(m_insn_log, "approx cost = %llu\n", (long long unsigned int)insn_cost);
#endif
   }
   else if (insn_cost > SubsecondTime::Zero() && instruction->getType() != INST_MEM_ACCESS)
   {
      // Handle all non-zero, non MemAccess instructions here

      // Mark this operation as a serialization instruction
      // It's cost needs to be added to the overall latency, and for accuracy
      //  it will help to be sure that the model knows about additional time
      //  used for overlapping events

      // These tend to be sync instructions
      // Because of timing issues (large synchronization deltas), take these latencies into account immediately
      //  Nevertheless, do not add the instruction cost into the interval model because the latency
      //  has already been taken into account here.  The interval model will serialize, flushing the old window

      std::vector<MicroOp> uops;
      uops.push_back(*m_serialize_uop);
      uops.back().setPeriod(m_state_insn_period);

      uint64_t new_latency_cycles;
      boost::tie(new_num_insns, new_latency_cycles) = simulate(uops);
      new_latency.addCycleLatency(new_latency_cycles);

      // Add the instruction cost immediately to prevent synchronization issues
      new_latency.addLatency(insn_cost);

      if (instruction->getType() == INST_SYNC)
      {
         // Keep track of the type of Sync instruction and it's latency to calculate CPI numbers
         SyncInstruction const* sync_insn = dynamic_cast<SyncInstruction const*>(instruction);
         LOG_ASSERT_ERROR(sync_insn != NULL, "Expected a SyncInstruction, but did not get one.");
         switch(sync_insn->getSyncType()) {
         case(SyncInstruction::FUTEX):
            m_cpiSyncFutex += insn_cost;
            break;
         case(SyncInstruction::PTHREAD_MUTEX):
            m_cpiSyncPthreadMutex += insn_cost;
            break;
         case(SyncInstruction::PTHREAD_COND):
            m_cpiSyncPthreadCond += insn_cost;
            break;
         case(SyncInstruction::PTHREAD_BARRIER):
            m_cpiSyncPthreadBarrier += insn_cost;
            break;
         case(SyncInstruction::JOIN):
            m_cpiSyncJoin += insn_cost;
            break;
         case(SyncInstruction::DVFS_TRANSITION):
            m_cpiSyncDvfsTransition += insn_cost;
            break;
         default:
            LOG_ASSERT_ERROR(false, "Unexpected SyncInstruction::type_t enum type. (%d)", sync_insn->getSyncType());
         }
         m_idle_elapsed_time.addLatency(insn_cost);
      }
      else if (instruction->getType() == INST_RECV)
      {
         RecvInstruction const* recv_insn = dynamic_cast<RecvInstruction const*>(instruction);
         LOG_ASSERT_ERROR(recv_insn != NULL, "Expected a RecvInstruction, but did not get one.");
         m_cpiRecv += insn_cost;
         m_idle_elapsed_time.addLatency(insn_cost);
      }
      else if (instruction->getType() == INST_TLB_MISS)
      {
         TLBMissInstruction const* tlb_miss_insn = dynamic_cast<TLBMissInstruction const*>(instruction);
         LOG_ASSERT_ERROR(tlb_miss_insn != NULL, "Expected a TLBMissInstruction, but did not get one.");
         if (tlb_miss_insn->isIfetch())
            m_cpiITLBMiss += insn_cost;
         else
            m_cpiDTLBMiss += insn_cost;
      }
      else
      {
         LOG_ASSERT_ERROR(false, "Unexpectedly received something other than a Sync, Recv or TLBMissInstruction");
      }


#if DEBUG_DYN_INSN_LOG
      fprintf(m_dyninsn_log, "[%llu] %s: cost = %llu\n", (long long unsigned int)m_elapsed_time, instruction->getTypeName().c_str(), (long long unsigned int)insn_cost);
#endif
   }
   else if (insn_cost > SubsecondTime::Zero())
   {

      // Handle non-zero MemAccess DynamicInstructions here

      // MemAccess instructions are memory overheads, and should be handled the same way that Long Latency Loads are
      // Currently, the simulator requires that latencies that return from the memory hierarchy go into effect
      // immediately.  If one tries to place these latencies as normal instructions into the interval model,
      // Graphite will still see a huge delta with this cpu's time, and then generate another, equally large
      // MemAccess instruction.  Therefore, as a work around, add the latencies to the

      MemAccessInstruction const* mem_dyn_insn = dynamic_cast<MemAccessInstruction const*>(instruction);
      LOG_ASSERT_ERROR(mem_dyn_insn != NULL, "Expected a MemAccessInstruction, but did not get one.");

      // Update uop with the necessary information for the MemAccess DynamicInstruction
      std::vector<MicroOp> uops;
      MicroOp* uop = new MicroOp();

      // Long latency load setup
      SubsecondTime cost_add_latency_now(SubsecondTime::Zero());
      uint32_t cost_add_latency_interval = 0;
      uint32_t cutoff = lll_info.getCutoff();
      bool force_lll = false;
      // if we are a long latency load (0 == disable)
      if ((cutoff > 0) & (insn_cost > cutoff * m_state_insn_period.getPeriod()))
      {
         // Long latency load
         cost_add_latency_now = insn_cost;
         cost_add_latency_interval = 1;
         // Force this instruction as a LLL, even though we will be taking into account the latencies right away
         // This will flush the old window and also check for 2nd order effects (nested LLLs)
         // FIXME For possible next steps, be sure to pass real LLL value in when we want to compare LLL event lengths for higher accuracy
         force_lll = true;
      }
      else
      {
         // Normal load
         cost_add_latency_now = SubsecondTime::Zero();
         cost_add_latency_interval = SubsecondTime::divideRounded(insn_cost, m_state_insn_period.getPeriod());
      }

      Memory::Access data_address;
      data_address.set(mem_dyn_insn->getDataAddress() & m_l1_line_mask); // Address to load.  Enforce L1 line dependencies here, as it does not happen in the cache model
      uop->makeLoad(
           0 // uop offset of 0 (first uop)
         , data_address
         , XED_ICLASS_INVALID // opcode
         , instruction->getTypeName()  // instructionName
         , cost_add_latency_interval // execLatency
      );
      uop->setPeriod(m_state_insn_period);
      uop->setForceLongLatencyLoad(force_lll);

      Memory::Access insn_address;
      insn_address.set(instruction->getAddress());
      uop->setInstructionPointer(insn_address);
      uop->setFirst(true);
      uop->setLast(true);
      // This load's value needs to be registered
      // Right now, serialization instructions are assumed to be executes, and the load latencies are skipped
      //uop.setSerializing( mem_dyn_insn->isFence() );
      // Add this micro-op to the vector for submission
      if ( mem_dyn_insn->isFence() )
      {
         // Add memory fencing support to better simulate actual conditions
         // CMPXCHG instructions are called in mutex handlers, and their performance is about the same as MFENCEs
         uops.push_back(*m_mfence_uop);
         uops.back().setPeriod(m_state_insn_period);
         //uops.push_back(serialize_uop);
         uops.push_back(*uop); // The period (CPU frequency) has already been set above
         uops.push_back(*m_mfence_uop);
         uops.back().setPeriod(m_state_insn_period);
         // Additionally, we need to think about serialization, and it's effect
         // In the case of a system call, the system will be serialized
         // This would matter only for the case when we initially don't have a lock
         //  and then we go into the OS only to get the lock and return without much
         //  wait.  In that case, the serialization effects could hit performance
         //  more than the memory barrier ones.
      }
      else
      {
         uops.push_back(*uop);
      }
      delete uop;

      // TODO Before simulating, iterate over the uops to mark them as first/last

      // Send this into the interval simulator
      uint64_t new_latency_cycles;
      boost::tie(new_num_insns, new_latency_cycles) = simulate(uops);
      new_latency.addCycleLatency(new_latency_cycles);

      // Add a potential LLL cost that needs to be registered right away
      new_latency.addLatency(cost_add_latency_now);

      m_dyninsn_count++;
      //m_dyninsn_cost+=insn_cost; // FIXME

      m_cpiMemAccess += cost_add_latency_now;

#if DEBUG_DYN_INSN_LOG
      fprintf(m_dyninsn_log, "[%llu](MA) %s: cost = %llu\n", (long long unsigned int) m_elapsed_time, instruction->getTypeName().c_str(), (long long unsigned int) insn_cost);
#endif
   }
   else
   {
      m_dyninsn_zero_count++;
   }
   m_instruction_count += new_num_insns;
   m_elapsed_time.addLatency(new_latency);

#if DEBUG_CYCLE_COUNT_LOG
   fprintf(m_cycle_log, "[%s] latency=%d\n", itostr(m_elapsed_time).c_str(), itostr(new_latency.getElapsedTime()).c_str());
#endif

   // At the end, update our state to process the next instruction
   resetState();

   return true;
}

boost::tuple<uint64_t,uint64_t> IntervalPerformanceModel::simulate(const std::vector<MicroOp>& insts)
{
   return interval_timer.simulate(insts);
}

void IntervalPerformanceModel::incrementElapsedTime(SubsecondTime time)
{
   m_cpiFastforwardTime += time;
   m_elapsed_time.addLatency(time);
}

void IntervalPerformanceModel::setElapsedTime(SubsecondTime time)
{
   LOG_ASSERT_ERROR((time >= m_elapsed_time.getElapsedTime()) || (m_elapsed_time.getElapsedTime() == SubsecondTime::Zero()),
         "time(%s) < m_elapsed_time(%s)",
         itostr(time).c_str(),
         itostr(m_elapsed_time.getElapsedTime()).c_str());
   if (m_elapsed_time.getElapsedTime() == SubsecondTime::Zero())
   {
      m_cpiStartTime += time;
   }
   else
   {
      LOG_PRINT_ERROR("This function should only be used for starting a new thread (SPAWN_INST)");
   }
   m_idle_elapsed_time.setElapsedTime(time - m_elapsed_time.getElapsedTime());
   m_elapsed_time.setElapsedTime(time);
}

void IntervalPerformanceModel::resetState()
{
   m_state_uops_done = false;
   m_state_icache_done = false;
   m_state_num_reads_done = 0;
   m_state_num_writes_done = 0;
   m_state_num_nonmem_done = 0;
   m_state_insn_period *= 0;
   m_state_instruction = NULL;
   m_current_uops.clear();
}
