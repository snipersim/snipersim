#include "core.h"
#include "performance_model.h"
#include "branch_predictor.h"
#include "simulator.h"
#include "simple_performance_model.h"
#include "iocoom_performance_model.h"
#include "magic_performance_model.h"
#include "oneipc_performance_model.h"
#include "interval_performance_model.h"
#include "core_manager.h"
#include "config.hpp"

PerformanceModel* PerformanceModel::create(Core* core)
{
   String type;

   try {
      type = Sim()->getCfg()->getString("perf_model/core/type");
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
      int mispredict_penalty = Sim()->getCfg()->getInt("perf_model/branch_predictor/mispredict_penalty",0);
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
   , m_hold(false)
   #ifdef ENABLE_PERF_MODEL_OWN_THREAD
   , m_basic_block_queue(64) // Reduce from default size to keep memory issue time more or less synchronized
   #else
   , m_basic_block_queue(128) // Need a bit more space for when the dyninsninfo items aren't coming in yet, or for a boatload of TLBMissInstructions
   #endif
   , m_dynamic_info_queue(640) // Required for REPZ CMPSB instructions with max counts of 256 (256 * 2 memory accesses + space for other dynamic instructions)
   , m_current_ins_index(0)
{
   m_bp = BranchPredictor::create(core->getId());
}

PerformanceModel::~PerformanceModel()
{
   delete m_bp;
}

void PerformanceModel::enable()
{
   // MCP perf model should never be enabled
   if (getCore()->getId() == Config::getSingleton()->getMCPCoreNum())
      return;
   if (!Config::getSingleton()->getEnablePerformanceModeling())
      return;

   m_enabled = true;
}

void PerformanceModel::disable()
{
   m_enabled = false;
}

void PerformanceModel::countInstructions(IntPtr address, UInt32 count)
{
}

void PerformanceModel::queueDynamicInstruction(Instruction *i)
{
   if (i->getType() == INST_SPAWN) {
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

      BasicBlock *bb = new BasicBlock(true);
      bb->push_back(i);
      #ifdef ENABLE_PERF_MODEL_OWN_THREAD
         m_basic_block_queue.push_wait(bb);
      #else
         m_basic_block_queue.push(bb);
      #endif
}

void PerformanceModel::queueBasicBlock(BasicBlock *basic_block)
{
   #ifdef ENABLE_PERF_MODEL_OWN_THREAD
      m_basic_block_queue.push_wait(basic_block);
   #else
      m_basic_block_queue.push(basic_block);
   #endif
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
      DynamicInstructionInfo i = m_dynamic_info_queue.pop_wait();
   #else
      DynamicInstructionInfo i = m_dynamic_info_queue.pop();
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

DynamicInstructionInfo* PerformanceModel::getDynamicInstructionInfo(const Instruction &instruction)
{
   DynamicInstructionInfo* info = getDynamicInstructionInfo();

   if (!info)
      return NULL;

   LOG_ASSERT_ERROR(info->eip == instruction.getAddress(), "Expected dynamic info for eip %lx \"%s\", got info for eip %lx", instruction.getAddress(), instruction.getDisassembly().c_str(), info->eip);

   if ((info->type == DynamicInstructionInfo::MEMORY_READ || info->type == DynamicInstructionInfo::MEMORY_WRITE)
      && info->memory_info.hit_where == HitWhere::UNKNOWN)
   {
      if (info->memory_info.executed) {
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
      } else {
         info->memory_info.latency = 1 * m_core->getDvfsDomain()->getPeriod(); // 1 cycle latency
         info->memory_info.hit_where = HitWhere::PREDICATE_FALSE;
      }
   }

   return info;
}
