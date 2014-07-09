#include "core.h"
#include "network.h"
#include "syscall_model.h"
#include "branch_predictor.h"
#include "memory_manager_base.h"
#include "performance_model.h"
#include "instruction.h"
#include "clock_skew_minimization_object.h"
#include "core_manager.h"
#include "dvfs_manager.h"
#include "hooks_manager.h"
#include "trace_manager.h"
#include "simulator.h"
#include "log.h"
#include "config.hpp"
#include "stats.h"
#include "topology_info.h"
#include "cheetah_manager.h"

#include <cstring>

#if 0
   extern Lock iolock;
#  define MYLOG(...) { ScopedLock l(iolock); fflush(stderr); fprintf(stderr, "[%8lu] %dcor %-25s@%03u: ", getPerformanceModel()->getCycleCount(ShmemPerfModel::_USER_THREAD), m_core_id, __FUNCTION__, __LINE__); fprintf(stderr, __VA_ARGS__); fprintf(stderr, "\n"); fflush(stderr); }
#else
#  define MYLOG(...) {}
#endif

#define VERBOSE 0

const char * ModeledString(Core::MemModeled modeled) {
   switch(modeled)
   {
      case Core::MEM_MODELED_NONE:           return "none";
      case Core::MEM_MODELED_COUNT:          return "count";
      case Core::MEM_MODELED_COUNT_TLBTIME:  return "count/tlb";
      case Core::MEM_MODELED_TIME:           return "time";
      case Core::MEM_MODELED_FENCED:         return "fenced";
      case Core::MEM_MODELED_RETURN:         return "return";
   }
  return "?";
}


const char * core_state_names[] = {
   "running",
   "initializing",
   "stalled",
   "sleeping",
   "waking_up",
   "idle",
   "broken",
};
static_assert(Core::NUM_STATES == sizeof(core_state_names) / sizeof(core_state_names[0]),
              "Not enough values in core_state_names");
const char * Core::CoreStateString(Core::State state)
{
   LOG_ASSERT_ERROR(state < Core::NUM_STATES, "Invalid core state %d", state);
   return core_state_names[state];
}


Lock Core::m_global_core_lock;
UInt64 Core::g_instructions_hpi_global = 0;
UInt64 Core::g_instructions_hpi_global_callback = 0;

Core::Core(SInt32 id)
   : m_core_id(id)
   , m_dvfs_domain(Sim()->getDvfsManager()->getCoreDomain(id))
   , m_thread(NULL)
   , m_bbv(id)
   , m_topology_info(new TopologyInfo(id))
   , m_cheetah_manager(Sim()->getCfg()->getBool("core/cheetah/enabled") ? new CheetahManager(id) : NULL)
   , m_core_state(Core::IDLE)
   , m_icache_last_block(-1)
   , m_spin_loops(0)
   , m_spin_instructions(0)
   , m_spin_elapsed_time(SubsecondTime::Zero())
   , m_instructions(0)
   , m_instructions_callback(UINT64_MAX)
   , m_instructions_hpi_callback(0)
   , m_instructions_hpi_last(0)
{
   LOG_PRINT("Core ctor for: %d", id);

   registerStatsMetric("core", id, "instructions", &m_instructions);
   registerStatsMetric("core", id, "spin_loops", &m_spin_loops);
   registerStatsMetric("core", id, "spin_instructions", &m_spin_instructions);
   registerStatsMetric("core", id, "spin_elapsed_time", &m_spin_elapsed_time);

   Sim()->getStatsManager()->logTopology("hwcontext", id, id);

   m_network = new Network(this);

   m_clock_skew_minimization_client = ClockSkewMinimizationClient::create(this);

   m_shmem_perf_model = new ShmemPerfModel();

   LOG_PRINT("instantiated memory manager model");
   m_memory_manager = MemoryManagerBase::createMMU(
         Sim()->getCfg()->getString("caching_protocol/type"),
         this, m_network, m_shmem_perf_model);

   m_performance_model = PerformanceModel::create(this);
}

Core::~Core()
{
   if (m_cheetah_manager)
      delete m_cheetah_manager;
   delete m_topology_info;
   delete m_memory_manager;
   delete m_shmem_perf_model;
   delete m_performance_model;
   if (m_clock_skew_minimization_client)
      delete m_clock_skew_minimization_client;
   delete m_network;
}

void Core::enablePerformanceModels()
{
   getShmemPerfModel()->enable();
   getMemoryManager()->enableModels();
   getNetwork()->enableModels();
   getPerformanceModel()->enable();
}

void Core::disablePerformanceModels()
{
   getShmemPerfModel()->disable();
   getMemoryManager()->disableModels();
   getNetwork()->disableModels();
   getPerformanceModel()->disable();
}

bool
Core::countInstructions(IntPtr address, UInt32 count)
{
   bool check_rescheduled = false;

   m_instructions += count;
   if (m_bbv.sample())
      m_bbv.count(address, count);
   m_performance_model->countInstructions(address, count);

   if (isEnabledInstructionsCallback())
   {
      if (m_instructions >= m_instructions_callback)
      {
         disableInstructionsCallback();
         Sim()->getHooksManager()->callHooks(HookType::HOOK_INSTR_COUNT, m_core_id);
         // When using the fast-forward performance model, HOOK_INSTR_COUNT may cause a rescheduling
         // of the current thread so let it know that it should make the appropriate checks
         check_rescheduled = true;
      }
   }

   hookPeriodicInsCheck();

   return check_rescheduled;
}

void
Core::hookPeriodicInsCheck()
{
   if (m_instructions > m_instructions_hpi_callback)
   {
      __sync_fetch_and_add(&g_instructions_hpi_global, m_instructions - m_instructions_hpi_last);
      m_instructions_hpi_callback += Sim()->getConfig()->getHPIInstructionsPerCore();
      m_instructions_hpi_last = m_instructions;

      // Quick, unlocked check if we should do the HOOK_PERIODIC_INS callback
      if (g_instructions_hpi_global > g_instructions_hpi_global_callback)
         hookPeriodicInsCall();
   }
}

void
Core::hookPeriodicInsCall()
{
   // Take the Thread lock, to make sure no other core calls us at the same time
   // and that the hook callback is also serialized w.r.t. other global events
   ScopedLock sl(Sim()->getThreadManager()->getLock());

   // Definitive, locked checked if we should do the HOOK_PERIODIC_INS callback
   if (g_instructions_hpi_global > g_instructions_hpi_global_callback)
   {
      Sim()->getHooksManager()->callHooks(HookType::HOOK_PERIODIC_INS, g_instructions_hpi_global);
      g_instructions_hpi_global_callback += Sim()->getConfig()->getHPIInstructionsGlobal();
   }
}

bool
Core::accessBranchPredictor(IntPtr eip, bool taken, IntPtr target)
{
   PerformanceModel *prfmdl = getPerformanceModel();
   BranchPredictor *bp = prfmdl->getBranchPredictor();

   if (bp)
   {
      bool prediction = bp->predict(eip, target);
      bp->update(prediction, taken, eip, target);
      return (prediction != taken);
   }
   else
   {
      return false;
   }
}

MemoryResult
makeMemoryResult(HitWhere::where_t _hit_where, SubsecondTime _latency)
{
   LOG_ASSERT_ERROR(_hit_where < HitWhere::NUM_HITWHERES, "Invalid HitWhere %u", (long)_hit_where);
   MemoryResult res;
   res.hit_where = _hit_where;
   res.latency = _latency;
   return res;
}

void
Core::logMemoryHit(bool icache, mem_op_t mem_op_type, IntPtr address, MemModeled modeled, IntPtr eip)
{
   getMemoryManager()->addL1Hits(icache, mem_op_type, 1);
}

MemoryResult
Core::readInstructionMemory(IntPtr address, UInt32 instruction_size)
{
   LOG_PRINT("Instruction: Address(0x%x), Size(%u), Start READ",
           address, instruction_size);

   UInt64 blockmask = ~(getMemoryManager()->getCacheBlockSize() - 1);
   bool single_cache_line = ((address & blockmask) == ((address + instruction_size - 1) & blockmask));

   // Assume the core reads full instruction cache lines and caches them internally for subsequent instructions.
   // This reduces L1-I accesses and power to more realistic levels.
   // For Nehalem, it's in fact only 16 bytes, other architectures (Sandy Bridge) have a micro-op cache,
   // so this is just an approximation.

   // When accessing the same cache line as last time, don't access the L1-I
   if ((address & blockmask) == m_icache_last_block)
   {
      if (single_cache_line)
      {
         return makeMemoryResult(HitWhere::L1I, getMemoryManager()->getL1HitLatency());
      }
      else
      {
         // Instruction spanning cache lines: drop the first line, do access the second one
         address = (address & blockmask) + getMemoryManager()->getCacheBlockSize();
      }
   }

   // Update the most recent cache line accessed
   m_icache_last_block = address & blockmask;

   // Cases with multiple cache lines or when we are not sure that it will be a hit call into the caches
   return initiateMemoryAccess(MemComponent::L1_ICACHE,
             Core::NONE, Core::READ, address & blockmask, NULL, getMemoryManager()->getCacheBlockSize(), MEM_MODELED_COUNT_TLBTIME, 0, SubsecondTime::MaxTime());
}

void Core::accessMemoryFast(bool icache, mem_op_t mem_op_type, IntPtr address)
{
   if (m_cheetah_manager && icache == false)
      m_cheetah_manager->access(mem_op_type, address);

   SubsecondTime latency = getMemoryManager()->coreInitiateMemoryAccessFast(icache, mem_op_type, address);

   if (latency > SubsecondTime::Zero())
      m_performance_model->handleMemoryLatency(latency, HitWhere::MISS);
}

MemoryResult
Core::initiateMemoryAccess(MemComponent::component_t mem_component,
      lock_signal_t lock_signal,
      mem_op_t mem_op_type,
      IntPtr address,
      Byte* data_buf, UInt32 data_size,
      MemModeled modeled,
      IntPtr eip,
      SubsecondTime now)
{
   MYLOG("access %lx+%u %c%c modeled(%s)", address, data_size, mem_op_type == Core::WRITE ? 'W' : 'R', mem_op_type == Core::READ_EX ? 'X' : ' ', ModeledString(modeled));

   if (data_size <= 0)
   {
      return makeMemoryResult((HitWhere::where_t)mem_component,SubsecondTime::Zero());
   }

   // Setting the initial time
   SubsecondTime initial_time = (now == SubsecondTime::MaxTime()) ? getPerformanceModel()->getElapsedTime() : now;

   // Protect from concurrent access by user thread (doing rewritten memops) and core thread (doing icache lookups)
   if (lock_signal != Core::UNLOCK)
      m_mem_lock.acquire();

#if 0
   static int i = 0;
   static Lock iolock;
   if ((i++) % 1000 == 0) {
      ScopedLock slio(iolock);
      printf("[TIME],%lu,", (Timer::now() / 100000) % 10000000);
      for(int i = 0; i < Sim()->getConfig()->getApplicationCores(); ++i)
        if (i == m_core_id)
          printf("%lu,%lu,%lu,", initial_time, getShmemPerfModel()->getCycleCount(ShmemPerfModel::_USER_THREAD), getShmemPerfModel()->getCycleCount(ShmemPerfModel::_SIM_THREAD));
        else
          printf(",,,");
      printf("\n");
   }
#endif

   getShmemPerfModel()->setElapsedTime(ShmemPerfModel::_USER_THREAD, initial_time);

   LOG_PRINT("Time(%s), %s - ADDR(0x%x), data_size(%u), START",
        itostr(initial_time).c_str(),
        ((mem_op_type == READ) ? "READ" : "WRITE"),
        address, data_size);

   UInt32 num_misses = 0;
   HitWhere::where_t hit_where = HitWhere::UNKNOWN;
   UInt32 cache_block_size = getMemoryManager()->getCacheBlockSize();

   IntPtr begin_addr = address;
   IntPtr end_addr = address + data_size;
   IntPtr begin_addr_aligned = begin_addr - (begin_addr % cache_block_size);
   IntPtr end_addr_aligned = end_addr - (end_addr % cache_block_size);
   Byte *curr_data_buffer_head = (Byte*) data_buf;

   for (IntPtr curr_addr_aligned = begin_addr_aligned; curr_addr_aligned <= end_addr_aligned; curr_addr_aligned += cache_block_size)
   {
      // Access the cache one line at a time
      UInt32 curr_offset;
      UInt32 curr_size;

      // Determine the offset
      if (curr_addr_aligned == begin_addr_aligned)
      {
         curr_offset = begin_addr % cache_block_size;
      }
      else
      {
         curr_offset = 0;
      }

      // Determine the size
      if (curr_addr_aligned == end_addr_aligned)
      {
         curr_size = (end_addr % cache_block_size) - (curr_offset);
         if (curr_size == 0)
         {
            continue;
         }
      }
      else
      {
         curr_size = cache_block_size - (curr_offset);
      }

      LOG_PRINT("Start InitiateSharedMemReq: ADDR(0x%x), offset(%u), curr_size(%u)", curr_addr_aligned, curr_offset, curr_size);

      if (m_cheetah_manager)
         m_cheetah_manager->access(mem_op_type, curr_addr_aligned);

      HitWhere::where_t this_hit_where = getMemoryManager()->coreInitiateMemoryAccess(
               mem_component,
               lock_signal,
               mem_op_type,
               curr_addr_aligned, curr_offset,
               data_buf ? curr_data_buffer_head : NULL, curr_size,
               modeled);

      if (hit_where != (HitWhere::where_t)mem_component)
      {
         // If it is a READ or READ_EX operation,
         // 'initiateSharedMemReq' causes curr_data_buffer_head
         // to be automatically filled in
         // If it is a WRITE operation,
         // 'initiateSharedMemReq' reads the data
         // from curr_data_buffer_head
         num_misses ++;
      }
      if (hit_where == HitWhere::UNKNOWN || (this_hit_where != HitWhere::UNKNOWN && this_hit_where > hit_where))
         hit_where = this_hit_where;

      LOG_PRINT("End InitiateSharedMemReq: ADDR(0x%x), offset(%u), curr_size(%u)", curr_addr_aligned, curr_offset, curr_size);

      // Increment the buffer head
      curr_data_buffer_head += curr_size;
   }

   // Get the final cycle time
   SubsecondTime final_time = getShmemPerfModel()->getElapsedTime(ShmemPerfModel::_USER_THREAD);
   LOG_ASSERT_ERROR(final_time >= initial_time,
         "final_time(%s) < initial_time(%s)",
         itostr(final_time).c_str(),
         itostr(initial_time).c_str());

   LOG_PRINT("Time(%s), %s - ADDR(0x%x), data_size(%u), END\n",
        itostr(final_time).c_str(),
        ((mem_op_type == READ) ? "READ" : "WRITE"),
        address, data_size);

   if (lock_signal != Core::LOCK)
      m_mem_lock.release();

   // Calculate the round-trip time
   SubsecondTime shmem_time = final_time - initial_time;

   switch(modeled)
   {
#if 0
      case MEM_MODELED_DYNINFO:
      {
         DynamicInstructionInfo info = DynamicInstructionInfo::createMemoryInfo(eip, true, shmem_time, address, data_size, (mem_op_type == WRITE) ? Operand::WRITE : Operand::READ, num_misses, hit_where);
            m_performance_model->pushDynamicInstructionInfo(info);
#endif

      case MEM_MODELED_TIME:
      case MEM_MODELED_FENCED:
         if (m_performance_model->isEnabled())
         {
            /* queue a fake instruction that will account for the access latency */
            PseudoInstruction *i = new MemAccessInstruction(shmem_time, address, data_size, modeled == MEM_MODELED_FENCED);
            m_performance_model->queuePseudoInstruction(i);
         }
         break;
      case MEM_MODELED_COUNT:
      case MEM_MODELED_COUNT_TLBTIME:
         if (shmem_time > SubsecondTime::Zero())
            m_performance_model->handleMemoryLatency(shmem_time, hit_where);
         break;
      case MEM_MODELED_NONE:
      case MEM_MODELED_RETURN:
         break;
   }

   if (modeled != MEM_MODELED_NONE)
   {
      getShmemPerfModel()->incrTotalMemoryAccessLatency(shmem_time);
   }

   LOG_ASSERT_ERROR(hit_where != HitWhere::UNKNOWN, "HitWhere == UNKNOWN");

   return makeMemoryResult(hit_where, shmem_time);
}

// FIXME: This should actually be 'accessDataMemory()'
/*
 * accessMemory (lock_signal_t lock_signal, mem_op_t mem_op_type, IntPtr d_addr, char* data_buffer, UInt32 data_size)
 *
 * Arguments:
 *   lock_signal :: NONE, LOCK, or UNLOCK
 *   mem_op_type :: READ, READ_EX, or WRITE
 *   d_addr :: address of location we want to access (read or write)
 *   data_buffer :: buffer holding data for WRITE or buffer which must be written on a READ
 *   data_size :: size of data we must read/write
 *
 * Return Value:
 *   number of misses :: State the number of cache misses
 */
MemoryResult
Core::accessMemory(lock_signal_t lock_signal, mem_op_t mem_op_type, IntPtr d_addr, char* data_buffer, UInt32 data_size, MemModeled modeled, IntPtr eip, SubsecondTime now, bool is_fault_mask)
{
   // In PINTOOL mode, if the data is requested, copy it to/from real memory
   if (data_buffer && !is_fault_mask)
   {
      if (Sim()->getConfig()->getSimulationMode() == Config::PINTOOL)
      {
         nativeMemOp (NONE, mem_op_type, d_addr, data_buffer, data_size);
      }
      else if (Sim()->getConfig()->getSimulationMode() == Config::STANDALONE)
      {
         Sim()->getTraceManager()->accessMemory(m_core_id, lock_signal, mem_op_type, d_addr, data_buffer, data_size);
      }
      data_buffer = NULL; // initiateMemoryAccess's data is not used
   }

   if (modeled == MEM_MODELED_NONE)
      return makeMemoryResult(HitWhere::UNKNOWN, SubsecondTime::Zero());
   else
      return initiateMemoryAccess(MemComponent::L1_DCACHE, lock_signal, mem_op_type, d_addr, (Byte*) data_buffer, data_size, modeled, eip, now);
}


MemoryResult
Core::nativeMemOp(lock_signal_t lock_signal, mem_op_t mem_op_type, IntPtr d_addr, char* data_buffer, UInt32 data_size)
{
   if (data_size <= 0)
   {
      return makeMemoryResult(HitWhere::UNKNOWN,SubsecondTime::Zero());
   }

   if (lock_signal == LOCK)
   {
      assert(mem_op_type == READ_EX);
      m_global_core_lock.acquire();
   }

   if ( (mem_op_type == READ) || (mem_op_type == READ_EX) )
   {
      applicationMemCopy ((void*) data_buffer, (void*) d_addr, (size_t) data_size);
   }
   else if (mem_op_type == WRITE)
   {
      applicationMemCopy ((void*) d_addr, (void*) data_buffer, (size_t) data_size);
   }

   if (lock_signal == UNLOCK)
   {
      assert(mem_op_type == WRITE);
      m_global_core_lock.release();
   }

   return makeMemoryResult(HitWhere::UNKNOWN,SubsecondTime::Zero());
}

__attribute__((weak)) void
applicationMemCopy(void *dest, const void *src, size_t n)
{
   memcpy(dest, src, n);
}

void
Core::emulateCpuid(UInt32 eax, UInt32 ecx, cpuid_result_t &res) const
{
   switch(eax)
   {
      case 0x0:
      {
         cpuid(0, 0, res);
         res.eax = std::max(UInt32(0xb), res.eax); // Maximum input eax: make sure 0xb is included
         break;
      }
      case 0x1:
      {
         // Return native results, except for CPU id
         cpuid(eax, ecx, res);
         res.ebx = (m_core_id << 24) | (Sim()->getConfig()->getApplicationCores() << 16) | (res.ebx &0xffff);
         break;
      }
      case 0xb:
      {
         // Extended Topology Enumeration Leaf
         switch(ecx)
         {
            case 0:
               // Level 0: SMT
               res.eax = TopologyInfo::SMT_SHIFT_BITS;
               res.ebx = m_topology_info->smt_count; // SMT threads / core
               res.ecx = ecx | (1 << 8); // Level type = SMT
               break;
            case 1:
               // Level 1: cores
               res.eax = TopologyInfo::PACKAGE_SHIFT_BITS;
               res.ebx = m_topology_info->smt_count * m_topology_info->core_count; // HW contexts / package
               res.ecx = ecx | (2 << 8); // Level type = Core
               break;
            default:
               // Invalid level
               res.eax = 0;
               res.ebx = 0;
               res.ecx = ecx;
               break;
         }
         res.edx = m_topology_info->apic_id;
         break;
      }
      default:
      {
         // Return native results (original cpuid instruction is deleted)
         cpuid(eax, ecx, res);
         break;
      }
   }

   #if VERBOSE
   printf("CPUID[%d]: %08x %08x => ", m_core_id, eax, ecx);
   printf("%08x %08x %08x %08x\n", res.eax, res.ebx, res.ecx, res.edx);
   #endif
}
