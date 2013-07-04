#include "core.h"
#include "performance_model.h"
#include "fastforward_performance_model.h"
#include "branch_predictor.h"
#include "simulator.h"
#include "simple_performance_model.h"
#include "iocoom_performance_model.h"
#include "magic_performance_model.h"
#include "oneipc_performance_model.h"
#include "interval_performance_model.h"
#include "core_manager.h"
#include "config.hpp"
#include "stats.h"
#include "dvfs_manager.h"
#include "instruction_tracer.h"

PerformanceModel* PerformanceModel::create(Core* core)
{
   String type;

   try {
      type = Sim()->getCfg()->getStringArray("perf_model/core/type", core->getId());
   } catch (...) {
      LOG_PRINT_ERROR("No perf model type provided.");
   }

   if (type == "iocoom")
      return new IOCOOMPerformanceModel(core);
   else if (type == "simple")
      return new SimplePerformanceModel(core);
   else if (type == "magic")
      return new MagicPerformanceModel(core);
   else if (type == "oneipc")
      return new OneIPCPerformanceModel(core);
   else if (type == "interval")
   {
      // The interval model needs the branch misprediction penalty
      int mispredict_penalty = Sim()->getCfg()->getIntArray("perf_model/branch_predictor/mispredict_penalty", core->getId());
      return new IntervalPerformanceModel(core, mispredict_penalty);
   }
   else
   {
      LOG_PRINT_ERROR("Invalid perf model type: %s", type.c_str());
      return NULL;
   }
}

// Public Interface
PerformanceModel::PerformanceModel(Core *core)
   : m_core(core)
   , m_enabled(false)
   , m_fastforward(false)
   , m_fastforward_model(new FastforwardPerformanceModel(core, this))
   , m_detailed_sync(true)
   , m_hold(false)
   , m_instruction_count(0)
   , m_elapsed_time(Sim()->getDvfsManager()->getCoreDomain(core->getId()))
   , m_idle_elapsed_time(Sim()->getDvfsManager()->getCoreDomain(core->getId()))
   #ifdef ENABLE_PERF_MODEL_OWN_THREAD
   , m_basic_block_queue(64) // Reduce from default size to keep memory issue time more or less synchronized
   #else
   , m_basic_block_queue(128) // Need a bit more space for when the dyninsninfo items aren't coming in yet, or for a boatload of TLBMissInstructions
   #endif
   , m_dynamic_info_queue(640) // Required for REPZ CMPSB instructions with max counts of 256 (256 * 2 memory accesses + space for other dynamic instructions)
   , m_current_ins_index(0)
{
   m_bp = BranchPredictor::create(core->getId());

   m_instruction_tracer = InstructionTracer::create(core->getId());

   registerStatsMetric("performance_model", core->getId(), "instruction_count", &m_instruction_count);

   registerStatsMetric("performance_model", core->getId(), "elapsed_time", &m_elapsed_time);
   registerStatsMetric("performance_model", core->getId(), "idle_elapsed_time", &m_idle_elapsed_time);

   registerStatsMetric("performance_model", core->getId(), "cpiStartTime", &m_cpiStartTime);

   registerStatsMetric("performance_model", core->getId(), "cpiSyncFutex", &m_cpiSyncFutex);
   registerStatsMetric("performance_model", core->getId(), "cpiSyncPthreadMutex", &m_cpiSyncPthreadMutex);
   registerStatsMetric("performance_model", core->getId(), "cpiSyncPthreadCond", &m_cpiSyncPthreadCond);
   registerStatsMetric("performance_model", core->getId(), "cpiSyncPthreadBarrier", &m_cpiSyncPthreadBarrier);
   registerStatsMetric("performance_model", core->getId(), "cpiSyncJoin", &m_cpiSyncJoin);
   registerStatsMetric("performance_model", core->getId(), "cpiSyncPause", &m_cpiSyncPause);
   registerStatsMetric("performance_model", core->getId(), "cpiSyncSleep", &m_cpiSyncSleep);
   registerStatsMetric("performance_model", core->getId(), "cpiSyncUnscheduled", &m_cpiSyncUnscheduled);
   registerStatsMetric("performance_model", core->getId(), "cpiSyncDvfsTransition", &m_cpiSyncDvfsTransition);

   registerStatsMetric("performance_model", core->getId(), "cpiRecv", &m_cpiRecv);
}

PerformanceModel::~PerformanceModel()
{
   delete m_bp;
   delete m_fastforward_model;
}

void PerformanceModel::enable()
{
   m_enabled = true;
}

void PerformanceModel::disable()
{
   m_enabled = false;
}

void PerformanceModel::countInstructions(IntPtr address, UInt32 count)
{
   if (m_fastforward)
   {
      m_fastforward_model->countInstructions(address, count);
   }
}

void PerformanceModel::queueDynamicInstruction(Instruction *i)
{
   if (i->getType() == INST_SPAWN)
   {
      SpawnInstruction const* spawn_insn = dynamic_cast<SpawnInstruction const*>(i);
      LOG_ASSERT_ERROR(spawn_insn != NULL, "Expected a SpawnInstruction, but did not get one.");
      setElapsedTime(spawn_insn->getTime());
      return;
   }

   if (!m_enabled)
   {
      // queueBasicBlock and pushDynamicInstructionInfo are not being called in fast-forward by using instrumentation modes
      // For queueDynamicInstruction, which are used all over the place, ignore them manually
      delete i;
      return;
   }

   if (i->isIdle())
   {
      handleIdleInstruction(i);
      delete i;
   }
   else
   {
      if (m_fastforward)
      {
         m_fastforward_model->queueDynamicInstruction(i);
      }
      else
      {
         BasicBlock *bb = new BasicBlock(true);
         bb->push_back(i);
         #ifdef ENABLE_PERF_MODEL_OWN_THREAD
            m_basic_block_queue.push_wait(bb);
         #else
            m_basic_block_queue.push(bb);
         #endif
      }
   }
}

void PerformanceModel::queueBasicBlock(BasicBlock *basic_block)
{
   #ifdef ENABLE_PERF_MODEL_OWN_THREAD
      m_basic_block_queue.push_wait(basic_block);
   #else
      m_basic_block_queue.push(basic_block);
   #endif
}

void PerformanceModel::handleIdleInstruction(Instruction *instruction)
{
   // If fast-forwarding without detailed synchronization, our fast-forwarding IPC
   // already contains idle periods so we can ignore these now
   if (m_fastforward && !m_detailed_sync)
      return;

   if (instruction->getType() == INST_SYNC)
   {
      // Keep track of the type of Sync instruction and it's latency to calculate CPI numbers
      SyncInstruction const* sync_insn = dynamic_cast<SyncInstruction const*>(instruction);
      LOG_ASSERT_ERROR(sync_insn != NULL, "Expected a SyncInstruction, but did not get one.");

      // Thread may wake up on a different core than where it went to sleep, and/or a different thread
      // may have run on this core while the thread was asleep
      // So, compute actual delay between our last local time and the time we're supposed to wake up
      SubsecondTime time_begin = getElapsedTime();
      SubsecondTime time_end = sync_insn->getTime();
      SubsecondTime insn_cost;

      if (time_end > time_begin)
         insn_cost = time_end - time_begin;
      else
         // Core may have executed other instructions already
         insn_cost = SubsecondTime::Zero();

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
      case(SyncInstruction::PAUSE):
         m_cpiSyncPause += insn_cost;
         break;
      case(SyncInstruction::SLEEP):
         m_cpiSyncSleep += insn_cost;
         break;
      case(SyncInstruction::UNSCHEDULED):
         m_cpiSyncUnscheduled += insn_cost;
         break;
      default:
         LOG_ASSERT_ERROR(false, "Unexpected SyncInstruction::type_t enum type. (%d)", sync_insn->getSyncType());
      }
      incrementIdleElapsedTime(insn_cost);
   }
   else if (instruction->getType() == INST_DELAY)
   {
      SubsecondTime insn_cost = instruction->getCost(getCore());
      DelayInstruction const* delay_insn = dynamic_cast<DelayInstruction const*>(instruction);
      LOG_ASSERT_ERROR(delay_insn != NULL, "Expected a DelayInstruction, but did not get one.");
      switch(delay_insn->getDelayType()) {
      case(DelayInstruction::DVFS_TRANSITION):
         m_cpiSyncDvfsTransition += insn_cost;
         break;
      default:
         LOG_ASSERT_ERROR(false, "Unexpected DelayInstruction::type_t enum type. (%d)", delay_insn->getDelayType());
      }
      incrementIdleElapsedTime(insn_cost);
   }
   else if (instruction->getType() == INST_RECV)
   {
      SubsecondTime insn_cost = instruction->getCost(getCore());
      __attribute__((unused)) RecvInstruction const* recv_insn = dynamic_cast<RecvInstruction const*>(instruction);
      LOG_ASSERT_ERROR(recv_insn != NULL, "Expected a RecvInstruction, but did not get one.");
      m_cpiRecv += insn_cost;
      incrementIdleElapsedTime(insn_cost);
   }
   else
   {
      LOG_PRINT_ERROR("Unexpectedly received something other than a Sync or Recv Instruction");
   }

   // Notify the fast-forward performance model
   if (m_fastforward)
      m_fastforward_model->notifyElapsedTimeUpdate();
}

void PerformanceModel::iterate()
{
   // Because we will sometimes not have info available (we will throw
   // a DynamicInstructionInfoNotAvailable), we need to be able to
   // continue from the middle of a basic block. m_current_ins_index
   // tracks which instruction we are currently on within the basic
   // block.

   #ifdef ENABLE_PERF_MODEL_OWN_THREAD
   while (m_basic_block_queue.size() > 0)
   #else
   while (m_basic_block_queue.size() > 1)
   #endif
   {
      // While the functional thread is waiting because of clock skew minimization, wait here as well
      #ifdef ENABLE_PERF_MODEL_OWN_THREAD
      while(m_hold)
         sched_yield();
      #endif

      BasicBlock *current_bb = m_basic_block_queue.front();

      for( ; m_current_ins_index < current_bb->size(); m_current_ins_index++)
      {
         Instruction *ins = current_bb->at(m_current_ins_index);
         LOG_ASSERT_ERROR(!ins->isIdle(), "Idle instructions should not make it here!");

         if (m_instruction_tracer)
            m_instruction_tracer->handleInstruction(ins);
         bool res = handleInstruction(ins);
         if (!res)
            // DynamicInstructionInfo not available
            return;
      }

      if (current_bb->isDynamic())
         delete current_bb;

      m_basic_block_queue.pop();
      m_current_ins_index = 0; // move to beginning of next bb
   }

   synchronize();
}

void PerformanceModel::synchronize()
{
   ClockSkewMinimizationClient *client = m_core->getClockSkewMinimizationClient();
   if (client)
      client->synchronize(SubsecondTime::Zero(), false);
}

void PerformanceModel::pushDynamicInstructionInfo(DynamicInstructionInfo &i)
{
   #ifdef ENABLE_PERF_MODEL_OWN_THREAD
      m_dynamic_info_queue.push_wait(i);
   #else
      m_dynamic_info_queue.push(i);
   #endif
}

void PerformanceModel::popDynamicInstructionInfo()
{
   #ifdef ENABLE_PERF_MODEL_OWN_THREAD
      m_dynamic_info_queue.pop_wait();
   #else
      m_dynamic_info_queue.pop();
   #endif
}

DynamicInstructionInfo* PerformanceModel::getDynamicInstructionInfo()
{
   // Information is needed to model the instruction, but isn't
   // available. This is handled in iterate() by returning early and
   // continuing from that instruction later.

   #ifdef ENABLE_PERF_MODEL_OWN_THREAD
      m_dynamic_info_queue.empty_wait();
   #else
      if (m_dynamic_info_queue.empty())
         return NULL;
   #endif

   return &m_dynamic_info_queue.front();
}

DynamicInstructionInfo* PerformanceModel::getDynamicInstructionInfo(const Instruction &instruction, bool exec_loads)
{
   DynamicInstructionInfo* info = getDynamicInstructionInfo();

   if (!info)
      return NULL;

   LOG_ASSERT_ERROR(info->eip == instruction.getAddress(), "Expected dynamic info for eip %lx \"%s\", got info for eip %lx", instruction.getAddress(), instruction.getDisassembly().c_str(), info->eip);

   if ((info->type == DynamicInstructionInfo::MEMORY_READ || info->type == DynamicInstructionInfo::MEMORY_WRITE)
      && info->memory_info.hit_where == HitWhere::UNKNOWN)
   {
      if (info->memory_info.executed)
      {
         if (exec_loads)
         {
            MemoryResult res = m_core->accessMemory(
               /*instruction.isAtomic()
                  ? (info->type == DynamicInstructionInfo::MEMORY_READ ? Core::LOCK : Core::UNLOCK)
                  :*/ Core::NONE, // Just as in pin/lite/memory_modeling.cc, make the second part of an atomic update implicit
               info->type == DynamicInstructionInfo::MEMORY_READ ? (instruction.isAtomic() ? Core::READ_EX : Core::READ) : Core::WRITE,
               info->memory_info.addr,
               NULL,
               info->memory_info.size,
               Core::MEM_MODELED_RETURN,
               instruction.getAddress()
            );
            info->memory_info.latency = res.latency;
            info->memory_info.hit_where = res.hit_where;
         }
         else
         {
            info->memory_info.latency = SubsecondTime::Zero();
            info->memory_info.hit_where = HitWhere::UNKNOWN;
         }
      }
      else
      {
         info->memory_info.latency = 1 * m_core->getDvfsDomain()->getPeriod(); // 1 cycle latency
         info->memory_info.hit_where = HitWhere::PREDICATE_FALSE;
      }
   }

   return info;
}

void PerformanceModel::incrementIdleElapsedTime(SubsecondTime time)
{
   // Advance the idle time
   m_idle_elapsed_time.addLatency(time);
   // Advance the total (non-idle + idle) time
   incrementElapsedTime(time);
   // Let the performance model know time has jumped
   notifyElapsedTimeUpdate();
   if (m_fastforward)
      m_fastforward_model->notifyElapsedTimeUpdate();
}

// Only called at the start of a new thread (SPAWN_INST)
void PerformanceModel::setElapsedTime(SubsecondTime time)
{
   LOG_ASSERT_ERROR(time >= getElapsedTime(), "setElapsedTime() cannot go backwards in time");

   SubsecondTime insn_cost = time - getElapsedTime();
   if (getElapsedTime() > SubsecondTime::Zero())
      // Core has run something before, account as unscheduled time
      m_cpiSyncUnscheduled += insn_cost;
   else
      // First thread to run on this core
      m_cpiStartTime += insn_cost;
   incrementIdleElapsedTime(insn_cost);
}
