#include "core.h"
#include "performance_model.h"
#include "fastforward_performance_model.h"
#include "branch_predictor.h"
#include "simulator.h"
#include "oneipc_performance_model.h"
#include "interval_performance_model.h"
#include "rob_performance_model.h"
#include "rob_smt_performance_model.h"
#include "core_manager.h"
#include "config.hpp"
#include "stats.h"
#include "dvfs_manager.h"
#include "instruction_tracer.h"
#include "dynamic_instruction.h"

PerformanceModel* PerformanceModel::create(Core* core)
{
   String type;

   try {
      type = Sim()->getCfg()->getStringArray("perf_model/core/type", core->getId());
   } catch (...) {
      LOG_PRINT_ERROR("No perf model type provided.");
   }

   if (type == "oneipc")
      return new OneIPCPerformanceModel(core);
   else if (type == "interval")
   {
      // The interval model needs the branch misprediction penalty
      int mispredict_penalty = Sim()->getCfg()->getIntArray("perf_model/branch_predictor/mispredict_penalty", core->getId());
      return new IntervalPerformanceModel(core, mispredict_penalty);
   }
   else if (type == "rob")
   {
      uint32_t smt_threads = Sim()->getCfg()->getIntArray("perf_model/core/logical_cpus", core->getId());
      if (smt_threads == 1)
         return new RobPerformanceModel(core);
      else
         return new RobSmtPerformanceModel(core);
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
   , m_dynins_alloc(DynamicInstruction::createAllocator())
   , m_enabled(false)
   , m_fastforward(false)
   , m_fastforward_model(new FastforwardPerformanceModel(core, this))
   , m_detailed_sync(true)
   , m_hold(false)
   , m_instruction_count(0)
   , m_elapsed_time(Sim()->getDvfsManager()->getCoreDomain(core->getId()))
   , m_idle_elapsed_time(Sim()->getDvfsManager()->getCoreDomain(core->getId()))
   #ifdef ENABLE_PERF_MODEL_OWN_THREAD
   , m_instruction_queue(256) // Reduce from default size to keep memory issue time more or less synchronized
   #else
   , m_instruction_queue(1024) // Need a bit more space for when the dyninsninfo items aren't coming in yet, or for a boatload of TLBMissInstructions
   #endif
   , m_current_ins_index(0)
{
   m_bp = BranchPredictor::create(core->getId());

   m_instruction_tracer = InstructionTracer::create(core);

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
   registerStatsMetric("performance_model", core->getId(), "cpiSyncSyscall", &m_cpiSyncSyscall);
   registerStatsMetric("performance_model", core->getId(), "cpiSyncUnscheduled", &m_cpiSyncUnscheduled);
   registerStatsMetric("performance_model", core->getId(), "cpiSyncDvfsTransition", &m_cpiSyncDvfsTransition);

   registerStatsMetric("performance_model", core->getId(), "cpiRecv", &m_cpiRecv);
}

PerformanceModel::~PerformanceModel()
{
   delete m_bp;
   delete m_fastforward_model;
   if (m_instruction_tracer)
      delete m_instruction_tracer;
}

void PerformanceModel::enable()
{
   if (!m_enabled)
      enableDetailedModel();
   m_enabled = true;
}

void PerformanceModel::disable()
{
   if (m_enabled)
      disableDetailedModel();
   m_enabled = false;
}

void PerformanceModel::countInstructions(IntPtr address, UInt32 count)
{
   if (m_fastforward)
   {
      m_fastforward_model->countInstructions(address, count);
   }
}

void PerformanceModel::handleMemoryLatency(SubsecondTime latency, HitWhere::where_t hit_where)
{
   if (m_fastforward)
   {
      m_fastforward_model->handleMemoryLatency(latency, hit_where);
   }
}

void PerformanceModel::handleBranchMispredict()
{
   if (m_fastforward)
   {
      m_fastforward_model->handleBranchMispredict();
   }
}

DynamicInstruction* PerformanceModel::createDynamicInstruction(Instruction *ins, IntPtr eip)
{
   return DynamicInstruction::alloc(m_dynins_alloc, ins, eip);
}

void PerformanceModel::queuePseudoInstruction(PseudoInstruction *i)
{
   if (i->getType() == INST_SPAWN)
   {
      SpawnInstruction const* spawn_insn = dynamic_cast<SpawnInstruction const*>(i);
      LOG_ASSERT_ERROR(spawn_insn != NULL, "Expected a SpawnInstruction, but did not get one.");
      setElapsedTime(spawn_insn->getTime());
      delete i;
      return;
   }

   if (!m_enabled)
   {
      // queueInstruction and pushDynamicInstructionInfo are not being called in fast-forward by using instrumentation modes
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
         m_fastforward_model->queuePseudoInstruction(i);
      }
      else
      {
         #ifdef ENABLE_PERF_MODEL_OWN_THREAD
            m_instruction_queue.push_wait(createDynamicInstruction(i, 0));
         #else
            m_instruction_queue.push(createDynamicInstruction(i, 0));
         #endif
      }
   }
}

void PerformanceModel::queueInstruction(DynamicInstruction *ins)
{
   if (m_fastforward || !m_enabled)
   {
      // Some threads may not have switched instrumentation mode yet even though we have left ROI
      // Ignore the instructions they send to avoid overflowing buffers
      delete ins;
      return;
   }

   #ifdef ENABLE_PERF_MODEL_OWN_THREAD
      m_instruction_queue.push_wait(ins);
   #else
      m_instruction_queue.push(ins);
   #endif
}

void PerformanceModel::handleIdleInstruction(PseudoInstruction *instruction)
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
      case(SyncInstruction::SYSCALL):
         m_cpiSyncSyscall += insn_cost;
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
   while (m_instruction_queue.size() > 0)
   {
      // While the functional thread is waiting because of clock skew minimization, wait here as well
      #ifdef ENABLE_PERF_MODEL_OWN_THREAD
      while(m_hold)
         sched_yield();
      #endif

      DynamicInstruction *ins = m_instruction_queue.front();

      LOG_ASSERT_ERROR(!ins->instruction->isIdle(), "Idle instructions should not make it here!");

      if (!m_fastforward && m_enabled)
         handleInstruction(ins);

      delete ins;

      m_instruction_queue.pop();
   }

   synchronize();
}

void PerformanceModel::synchronize()
{
   ClockSkewMinimizationClient *client = m_core->getClockSkewMinimizationClient();
   if (client)
      client->synchronize(SubsecondTime::Zero(), false);
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
