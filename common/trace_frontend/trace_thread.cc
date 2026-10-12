#include "trace_thread.h"
#include "trace_manager.h"
#include "simulator.h"
#include "core_manager.h"
#include "thread_manager.h"
#include "thread.h"
#include "dvfs_manager.h"
#include "instruction.h"
#include "dynamic_instruction.h"
#include "performance_model.h"
#include "instruction_decoder_wlib.h"
#include "micro_op.h"
#include "config.hpp"
#include "syscall_model.h"
#include "core.h"
#include "magic_client.h"
#include "branch_predictor.h"
#include "rng.h"
#include "routine_tracer.h"
#include "sim_api.h"
#include "mimicos.h"
#include "stats.h"
#include "parametric_dram_directory_msi/memory_manager.h"

#include <unistd.h>
#include <sys/syscall.h>
#include <sched.h>
#include <errno.h>
#include <stdio.h>

#include <x86_decoder.h>  // TODO remove when the decode function in microop perf model is adapted

#include "debug_config.h"
#include <sstream>
#include <vector>

// New workers inherit their creator's affinity. Allocate from the startup
// CPU pool rather than a mask already narrowed by another trace worker.
static void pinTraceThreadToCPU(int thread_id)
{
   const std::vector<int> &cpus = Sim()->getTraceManager()->getHostCpus();

   if (cpus.empty())
      return;

   // Reuse CPUs when there are more workers than available CPUs.
   int target = cpus[thread_id % cpus.size()];
   cpu_set_t mask;
   CPU_ZERO(&mask);
   CPU_SET(target, &mask);
   if (sched_setaffinity(0, sizeof(mask), &mask) < 0)
      fprintf(stderr, "[TraceThread] Cannot pin thread %d to host CPU %d: errno=%d\n",
              thread_id, target, errno);
}

int TraceThread::m_isa = 0;

TraceThread::TraceThread(Thread *thread, SubsecondTime time_start, String tracefile, String responsefile, app_id_t app_id, bool cleanup)
   : m__thread(NULL)
   , m_thread(thread)
   , m_time_start(time_start)
   , m_trace(tracefile.c_str(), responsefile.c_str(), thread->getId())
   , m_kernel_trace(NULL)
   , m_app_trace(NULL)
   , m_current_sift_reader(NULL)
   , m_trace_has_pa(false)
   , m_champsim_trace(false)
   , m_champsim_access_size(Sift::CHAMPSIM_DEFAULT_MEM_SIZE)
   , m_address_randomization(Sim()->getCfg()->getBool("traceinput/address_randomization"))
   , m_appid_from_coreid(Sim()->getCfg()->getString("scheduler/type") == "sequential" ? true : false)
   , m_stop(false)
   , m_bbv_base(0)
   , m_bbv_count(0)
   , m_bbv_last(0)
   , m_bbv_end(false)
   , m_output_leftover_size(0)
   , m_tracefile(tracefile)
   , m_responsefile(responsefile)
   , m_app_id(app_id)
   , m_blocked(false)
   , m_cleanup(cleanup)
   , m_started(false)
   , m_stopped(false)
   , m_champsim_icache_hits(0)
   , m_champsim_icache_misses(0)
{

   bool userspace_mimicos_enabled = Sim()->getCfg()->getBool("general/enable_userspace_mimicos");

   if (userspace_mimicos_enabled)
   {
      m_current_run_func = &TraceThread::m_run_func_with_userpace_mimicos;
   }
   else
   {
      m_current_run_func = &TraceThread::m_run_func_default;
   }

   if (Sim()->getCfg()->hasKey("traceinput/champsim_mem_access_bytes"))
   {
      SInt64 configured_size = Sim()->getCfg()->getInt("traceinput/champsim_mem_access_bytes");
      if (configured_size > 0)
         m_champsim_access_size = static_cast<UInt32>(configured_size);
      else
         m_champsim_access_size = Sift::CHAMPSIM_DEFAULT_MEM_SIZE;
   }

   m_trace.setHandleInstructionCountFunc(TraceThread::__handleInstructionCountFunc, this);
   m_trace.setHandleCacheOnlyFunc(TraceThread::__handleCacheOnlyFunc, this);
   if (Sim()->getCfg()->getBool("traceinput/mirror_output"))
      m_trace.setHandleOutputFunc(TraceThread::__handleOutputFunc, this);
   m_trace.setHandleSyscallFunc(TraceThread::__handleSyscallFunc, this);
   m_trace.setHandleNewThreadFunc(TraceThread::__handleNewThreadFunc, this);
   m_trace.setHandleJoinFunc(TraceThread::__handleJoinFunc, this);
   m_trace.setHandleMagicFunc(TraceThread::__handleMagicFunc, this);
   m_trace.setHandleEmuFunc(TraceThread::__handleEmuFunc, this);
   m_trace.setHandleForkFunc(TraceThread::__handleForkFunc, this);
   if (Sim()->getRoutineTracer())
      m_trace.setHandleRoutineFunc(TraceThread::__handleRoutineChangeFunc, TraceThread::__handleRoutineAnnounceFunc, this);

   if (m_address_randomization)
   {
      // Fisher-Yates shuffle, simultaneously initializing array to m_address_randomization_table[i] = i
      // See http://en.wikipedia.org/wiki/Fisher%E2%80%93Yates_shuffle#The_.22inside-out.22_algorithm
      // By using the app_id as a random seed, we get an app_id-specific pseudo-random permutation of 0..255
      UInt64 state = rng_seed(app_id);
      m_address_randomization_table[0] = 0;
      for(unsigned int i = 1; i < 256; ++i)
      {
         uint8_t j = rng_next(state) % (i + 1);
         m_address_randomization_table[i] = m_address_randomization_table[j];
         m_address_randomization_table[j] = i;
      }
   }

   thread->setVa2paFunc(_va2pa, (UInt64)this);
   stats.kernel_time = SubsecondTime::Zero();

   // Guard against duplicate registration when a trace is restarted
   // (--sim-end=last-restart creates a new TraceThread with the same app_id).
   StatsMetricBase *existing = Sim()->getStatsManager()->getMetricObject("trace_thread", m_app_id, "kernel_time");
   if (existing == NULL)
      registerStatsMetric("trace_thread", m_app_id, "kernel_time", &stats.kernel_time);
   else
      static_cast<StatsMetric<SubsecondTime>*>(existing)->metric = &stats.kernel_time;
}

TraceThread::~TraceThread()
{
   delete m__thread;
   if (m_cleanup)
   {
      unlink(m_tracefile.c_str());
      unlink(m_responsefile.c_str());
   }
   for(std::unordered_map<IntPtr, const dl::DecodedInst *>::iterator i = m_decoder_cache.begin() ; i != m_decoder_cache.end() ; ++i)
   {
      delete (*i).second;
   }
   
   // Clean up ChampSim instruction cache (since TraceManager doesn't delete TraceThreads)
   cleanupChampSimCache();
}

void TraceThread::cleanupChampSimCache()
{
   // Already cleaned up?
   if (m_champsim_icache.empty()) {
      return;
   }
   
   // Print ChampSim instruction cache statistics
   UInt64 total_accesses = m_champsim_icache_hits + m_champsim_icache_misses;
   if (total_accesses > 0 || m_champsim_icache.size() > 0) {
      double hit_rate = (total_accesses > 0) ? (100.0 * m_champsim_icache_hits / total_accesses) : 0.0;
      printf("[CHAMPSIM_ICACHE] Stats: hits=%lu misses=%lu total=%lu hit_rate=%.2f%% cache_size=%zu\n",
             m_champsim_icache_hits, m_champsim_icache_misses, total_accesses, hit_rate, m_champsim_icache.size());
      fflush(stdout);
   }
   
   #if DEBUG_CHAMPSIM_CACHE >= DEBUG_BASIC
   printf("[CHAMPSIM_CACHE] cleanupChampSimCache: Cleaning up %zu cached ChampSim instructions\n", m_champsim_icache.size());
   #endif
   
   // Clean up ChampSim instruction cache
   size_t cleanup_count = 0;
   for (auto& kv : m_champsim_icache)
   {
      Instruction* instr = kv.second;
      if (instr) {
         const std::vector<const MicroOp*>* uops = instr->getMicroOps();
         #if DEBUG_CHAMPSIM_CACHE >= DEBUG_DETAILED
         printf("[CHAMPSIM_CACHE] FREE Instruction*=%p with %zu MicroOps\n", 
                (void*)instr, uops ? uops->size() : 0);
         #endif
         if (uops) {
            for (const MicroOp* uop : *uops) {
               delete uop;
            }
            delete uops;
         }
         delete instr;
         cleanup_count++;
      }
   }
   m_champsim_icache.clear();
   
   #if DEBUG_CHAMPSIM_CACHE >= DEBUG_BASIC
   printf("[CHAMPSIM_CACHE] cleanupChampSimCache: Freed %zu instructions\n", cleanup_count);
   #endif
}

UInt64 TraceThread::va2pa(UInt64 va, bool *noMapping)
{
   if (m_trace_has_pa)
   {
      UInt64 pa = m_trace.va2pa(va);
      if (pa != 0)
      {
         return pa;
      }
      else
      {
         if (noMapping)
            *noMapping = true;
         //else
         //   LOG_PRINT_WARNING("No mapping found for logical address %lx", va);
         // Fall through to construct an address with our thread id in the upper bits (assume address is private)
      }
   }

   UInt64 haddr;

   // When the scheduler is set to sequential, every thread with same core affinity
   // will be considered chunks of the same process, therefore they have the same
   // physical address space.
   if (m_appid_from_coreid)
   {
        haddr = UInt64(m_thread->getCore()->getId());
   }
   else
   {
        haddr = UInt64(m_thread->getAppId());
   }

   if (m_address_randomization)
   {
      // Set 16 bits to app_id | remap middle 36 bits using app_id-specific mapping | keep lower 12 bits (page offset)
      return (haddr << pa_core_shift) | (remapAddress(va >> va_page_shift) << va_page_shift) | (va & va_page_mask);
   }
   else
   {
      // Set 16 bits to app_id | keep lower 48 bits
      return (haddr << pa_core_shift) | (va & pa_va_mask);
   }
}

UInt64 TraceThread::remapAddress(UInt64 va_page)
{
   // va is the virtual address shifted right by the page size
   // By randomly remapping the lower 24 bits of va_page, addresses will be distributed
   // over a 1<<(16+3*8) = 64 GB range which should avoid artificial set contention in all cache levels.
   // Of course we want the remapping to be invertible so we never map different incoming addresses
   // onto the same outgoing address. This is guaranteed since m_address_randomization_table
   // contains each 0..255 number only once.
   UInt64 result = va_page;
   uint8_t *array = (uint8_t *)&result;
   array[0] = m_address_randomization_table[array[0]];
   array[1] = m_address_randomization_table[array[1]];
   array[2] = m_address_randomization_table[array[2]];
   return result;
}

void TraceThread::handleOutputFunc(uint8_t fd, const uint8_t *data, uint32_t size)
{
   FILE *fp;
   if (fd == 1)
      fp = stdout;
   else if (fd == 2)
      fp = stderr;
   else
      return;

   while(size)
   {
      const uint8_t* ptr = data;
      while(ptr < data + size && *ptr != '\r' && *ptr != '\n') ++ptr;
      if (ptr == data + size)
      {
         if (size > sizeof(m_output_leftover))
            size = sizeof(m_output_leftover);
         memcpy(m_output_leftover, data, size);
         m_output_leftover_size = size;
         break;
      }

      fprintf(fp, "[TRACE:%u] ", m_thread->getId());
      if (m_output_leftover_size)
      {
         fwrite(m_output_leftover, m_output_leftover_size, 1, fp);
         m_output_leftover_size = 0;
      }
      fwrite(data, ptr - data, 1, fp);
      fprintf(fp, "\n");

      while(ptr < data + size && (*ptr == '\r' || *ptr == '\n')) ++ptr;
      size -= (ptr - data);
      data = ptr;
   }
}

uint64_t TraceThread::handleSyscallFunc(uint16_t syscall_number, const uint8_t *data, uint32_t size)
{
   // We may have been blocked in a system call, if we start executing instructions again that means we're continuing
   if (m_blocked)
   {
      unblock();
   }

   LOG_ASSERT_ERROR(m_thread->getCore(), "Cannot execute while not on a core");
   uint64_t ret = 0;

   switch(syscall_number)
   {
      case SYS_exit_group:
         Sim()->getTraceManager()->endApplication(this, getCurrentTime());
         break;

      default:
      {
         LOG_ASSERT_ERROR(size == sizeof(SyscallMdl::syscall_args_t), "Syscall arguments not the correct size");

         SyscallMdl::syscall_args_t *args = (SyscallMdl::syscall_args_t *) data;

         m_blocked = m_thread->getSyscallMdl()->runEnter(syscall_number, *args);
         if (m_blocked == false)
         {
            ret = m_thread->getSyscallMdl()->runExit(ret);
         }
         break;
      }
   }

   return ret;
}

int32_t TraceThread::handleNewThreadFunc()
{
   return Sim()->getTraceManager()->createThread(m_app_id, getCurrentTime(), m_thread->getId());
}

int32_t TraceThread::handleForkFunc()
{
   return Sim()->getTraceManager()->createApplication(getCurrentTime(), m_thread->getId());
}

int32_t TraceThread::handleJoinFunc(int32_t join_thread_id)
{
   Sim()->getThreadManager()->joinThread(m_thread->getId(), join_thread_id);
   return 0;
}

uint64_t TraceThread::handleMagicFunc(uint64_t a, uint64_t b, uint64_t c)
{
   return handleMagicInstruction(m_thread->getId(), a, b, c);
}

void TraceThread::handleRoutineChangeFunc(Sift::RoutineOpType event, uint64_t eip, uint64_t esp, uint64_t callEip)
{
   switch(event)
   {
      case Sift::RoutineEnter:
         m_thread->getRoutineTracer()->routineEnter(eip, esp, callEip);
         break;
      case Sift::RoutineExit:
         m_thread->getRoutineTracer()->routineExit(eip, esp);
         break;
      case Sift::RoutineAssert:
         m_thread->getRoutineTracer()->routineAssert(eip, esp);
         break;
      default:
         LOG_PRINT_ERROR("Invalid Sift::RoutineOpType %d", event);
   }
}

bool TraceThread::handleEmuFunc(Sift::EmuType type, Sift::EmuRequest &req, Sift::EmuReply &res)
{
   // We may have been blocked in a system call, if we start executing instructions again that means we're continuing
   if (m_blocked)
   {
      unblock();
   }

   LOG_ASSERT_ERROR(m_thread->getCore(), "Cannot execute while not on a core");

   switch(type)
   {
      case Sift::EmuTypeRdtsc:
      {
         SubsecondTime cycles_fs = getCurrentTime();
         // Convert SubsecondTime to cycles in global clock domain
         const ComponentPeriod *dom_global = Sim()->getDvfsManager()->getGlobalDomain();
         UInt64 cycles = SubsecondTime::divideRounded(cycles_fs, *dom_global);

         res.rdtsc.cycles = cycles;
         return true;
      }
      case Sift::EmuTypeGetProcInfo:
      {
         res.getprocinfo.procid = m_thread->getCore()->getId();
         res.getprocinfo.nprocs = Sim()->getConfig()->getApplicationCores();
         res.getprocinfo.emunprocs = Sim()->getConfig()->getOSEmuNprocs() ? Sim()->getConfig()->getOSEmuNprocs() : Sim()->getConfig()->getApplicationCores();
         return true;
      }
      case Sift::EmuTypeGetTime:
      {
         res.gettime.time_ns = Sim()->getConfig()->getOSEmuTimeStart() * 1000000000
                             + getCurrentTime().getNS();
         return true;
      }
      case Sift::EmuTypeCpuid:
      {
         cpuid_result_t result;
         m_thread->getCore()->emulateCpuid(req.cpuid.eax, req.cpuid.ecx, result);
         res.cpuid.eax = result.eax;
         res.cpuid.ebx = result.ebx;
         res.cpuid.ecx = result.ecx;
         res.cpuid.edx = result.edx;
         return true;
      }
      case Sift::EmuTypeSetThreadInfo:
      {
         m_thread->m_os_info.tid = req.setthreadinfo.tid;
         return true;
      }
      case Sift::EmuTypePAPIstart:
      {
        m_papi_counters = new long long[NUM_PAPI_COUNTERS];
        for(unsigned int i = 0; i < NUM_PAPI_COUNTERS; i++)
          m_papi_counters[i] = 0;
        return true;
      }
      case Sift::EmuTypePAPIread:
      {
        m_papi_counters[PAPI_TOT_INS] = m_thread->getCore()->getPerformanceModel()->getInstructionCount();

        SubsecondTime cycles_fs = getCurrentTime();
        // Convert SubsecondTime to cycles in global clock domain
        const ComponentPeriod *dom_global = Sim()->getDvfsManager()->getGlobalDomain();
        UInt64 cycles = SubsecondTime::divideRounded(cycles_fs, *dom_global);

        m_papi_counters[PAPI_TOT_CYC] = cycles;

        UInt64 load_misses_l1d = Sim()->getStatsManager()->getMetricObject("L1-D", m_thread->getCore()->getId(), "load-misses")->recordMetric();
        UInt64 store_misses_l1d= Sim()->getStatsManager()->getMetricObject("L1-D", m_thread->getCore()->getId(), "store-misses")->recordMetric();

        UInt64 load_misses_l2  = Sim()->getStatsManager()->getMetricObject("L2", m_thread->getCore()->getId(), "load-misses")->recordMetric();
        UInt64 store_misses_l2 = Sim()->getStatsManager()->getMetricObject("L2", m_thread->getCore()->getId(), "store-misses")->recordMetric();

        UInt64 load_misses_l3  = Sim()->getStatsManager()->getMetricObject("L3", m_thread->getCore()->getId(), "load-misses")->recordMetric();
        UInt64 store_misses_l3 = Sim()->getStatsManager()->getMetricObject("L3", m_thread->getCore()->getId(), "store-misses")->recordMetric();


        m_papi_counters[PAPI_L1_DCM] = load_misses_l1d + store_misses_l1d;
        m_papi_counters[PAPI_L2_DCM] = load_misses_l2 + store_misses_l2;
        m_papi_counters[PAPI_L3_TCM] = load_misses_l3 + store_misses_l3;

        m_papi_counters[PAPI_BR_MSP] = m_thread->getCore()->getPerformanceModel()->getBranchPredictor()->getNumIncorrectPredictions();

        for(unsigned i = 0; i < NUM_PAPI_COUNTERS; i++)
          res.papi.values[i] = m_papi_counters[i];

        return true;
      }
      default:
         // Not emulated
         return false;
   }
}

void TraceThread::handleRoutineAnnounceFunc(uint64_t eip, const char *name, const char *imgname, uint64_t offset, uint32_t line, uint32_t column, const char *filename)
{
   Sim()->getRoutineTracer()->addRoutine(eip, name, imgname, offset, column, line, filename);
}

SubsecondTime TraceThread::getCurrentTime() const
{
   LOG_ASSERT_ERROR(m_thread->getCore() != NULL, "Cannot get time while not on a core");
   return m_thread->getCore()->getPerformanceModel()->getElapsedTime();
}

Instruction* TraceThread::decode(Sift::Instruction &inst)
{

   //printf("PC: %lx Size: %d num_addresses=%d is_branch=%d\n", inst.sinst->addr, inst.sinst->size, inst.num_addresses, inst.is_branch);
   if (m_decoder_cache.count(inst.sinst->addr) == 0)
      m_decoder_cache[inst.sinst->addr] = staticDecode(inst);
   
   const dl::DecodedInst& dec_inst = *(m_decoder_cache[inst.sinst->addr]);

   OperandList list;

   // Ignore memory-referencing operands in NOP instructions
   if (!(dec_inst.is_nop()))
   {
      for(uint32_t mem_idx = 0; mem_idx < Sim()->getDecoder()->num_memory_operands(&dec_inst); ++mem_idx)
         if (Sim()->getDecoder()->op_read_mem(&dec_inst, mem_idx))
            list.push_back(Operand(Operand::MEMORY, 0, Operand::READ));

      for(uint32_t mem_idx = 0; mem_idx < Sim()->getDecoder()->num_memory_operands(&dec_inst); ++mem_idx)
         if (Sim()->getDecoder()->op_write_mem(&dec_inst, mem_idx))
            list.push_back(Operand(Operand::MEMORY, 0, Operand::WRITE));
   }

   Instruction *instruction;
   if (inst.is_branch)
     instruction = new BranchInstruction(list); 

   else
      instruction = new GenericInstruction(list);

   instruction->setAddress(va2pa(inst.sinst->addr));
   instruction->setSize(inst.sinst->size);
   instruction->setAtomic(dec_inst.is_atomic());
   instruction->setDisassembly(dec_inst.disassembly_to_str().c_str());
   instruction->setDynamic(false);
   
   const std::vector<const MicroOp*> *uops = InstructionDecoder::decode(inst.sinst->addr, &dec_inst, instruction);
   instruction->setMicroOps(uops);

   return instruction;
}

Instruction* TraceThread::decodeChampsim(Sift::Instruction &inst)
{
   // Build cache key from instruction signature
   ChampSimCacheKey cache_key;
   cache_key.pc = inst.sinst->addr;
   cache_key.is_branch = inst.is_branch ? 1 : 0;
   cache_key.num_src_regs = inst.num_src_registers;
   cache_key.num_dest_regs = inst.num_dest_registers;
   cache_key.num_loads = inst.num_src_addresses;
   cache_key.num_stores = inst.num_dest_addresses;
   
   // Check cache first
   auto it = m_champsim_icache.find(cache_key);
   if (it != m_champsim_icache.end()) {
      // Cache hit - return cached instruction
      m_champsim_icache_hits++;
      #if DEBUG_CHAMPSIM_CACHE >= DEBUG_BASIC
      printf("[CHAMPSIM_CACHE] HIT  pc=0x%lx branch=%d src_regs=%d dest_regs=%d loads=%d stores=%d -> Instruction*=%p (cache_size=%zu)\n",
             cache_key.pc, cache_key.is_branch, cache_key.num_src_regs, cache_key.num_dest_regs,
             cache_key.num_loads, cache_key.num_stores, (void*)it->second, m_champsim_icache.size());
      #endif
      return it->second;
   }
   
   m_champsim_icache_misses++;
   
   #if DEBUG_CHAMPSIM_CACHE >= DEBUG_BASIC
   printf("[CHAMPSIM_CACHE] MISS pc=0x%lx branch=%d src_regs=%d dest_regs=%d loads=%d stores=%d (cache_size=%zu) -> ALLOCATING\n",
          cache_key.pc, cache_key.is_branch, cache_key.num_src_regs, cache_key.num_dest_regs,
          cache_key.num_loads, cache_key.num_stores, m_champsim_icache.size());
   #endif
   
   // Cache miss - create new instruction
   OperandList list;

   for (uint8_t idx = 0; idx < inst.num_src_registers; ++idx)
      list.push_back(Operand(Operand::REG, inst.src_registers[idx], Operand::READ));

   for (uint8_t idx = 0; idx < inst.num_dest_registers; ++idx)
      list.push_back(Operand(Operand::REG, inst.dest_registers[idx], Operand::WRITE));

   for (uint8_t idx = 0; idx < inst.num_src_addresses; ++idx)
      list.push_back(Operand(Operand::MEMORY, 0, Operand::READ));

   for (uint8_t idx = 0; idx < inst.num_dest_addresses; ++idx)
      list.push_back(Operand(Operand::MEMORY, 0, Operand::WRITE));

   Instruction *instruction = inst.is_branch ? static_cast<Instruction*>(new BranchInstruction(list))
                                             : static_cast<Instruction*>(new GenericInstruction(list));

   instruction->setAddress(va2pa(inst.sinst->addr));
   instruction->setSize(inst.sinst->size);
   instruction->setAtomic(false);
   instruction->setDisassembly("champsim-trace");
   // Cached instructions are NOT dynamic - they persist and are reused
   instruction->setDynamic(false);

   UInt32 mem_size = m_champsim_access_size ? m_champsim_access_size : Sift::CHAMPSIM_DEFAULT_MEM_SIZE;

   size_t num_loads = inst.num_src_addresses;
   size_t num_stores = inst.num_dest_addresses;
   size_t num_execs = 1;
   size_t total_microops = num_loads + num_execs + num_stores;
   if (total_microops == 0)
   {
      num_execs = 1;
      total_microops = 1;
   }

   auto *uops = new std::vector<const MicroOp*>();
   uops->reserve(total_microops);

   for (size_t idx = 0; idx < total_microops; ++idx)
   {
      MicroOp *uop = new MicroOp();
      uop->setInstructionPointer(Memory::make_access(inst.sinst->addr));
      uop->setOperandSize(mem_size * 8);
      uop->setInstruction(instruction);
      uop->setDecodedInstruction(NULL);

      if (idx < num_loads)
      {
         uop->makeLoad(idx, dl::Decoder::DL_OPCODE_INVALID, "champsim-load", mem_size);
      }
      else if (idx < num_loads + num_execs)
      {
         size_t exec_index = idx - num_loads;
         uop->makeExecute(exec_index, num_loads, dl::Decoder::DL_OPCODE_INVALID, "champsim-exec", inst.is_branch);
      }
      else
      {
         size_t store_index = idx - num_loads - num_execs;
         uop->makeStore(store_index, num_execs, dl::Decoder::DL_OPCODE_INVALID, "champsim-store", mem_size);
      }

      if (idx == 0)
         uop->setFirst(true);
      if (idx == total_microops - 1)
         uop->setLast(true);

      uops->push_back(uop);
   }

   instruction->setMicroOps(uops);
   
   // Add to cache for future reuse
   m_champsim_icache[cache_key] = instruction;
   
   #if DEBUG_CHAMPSIM_CACHE >= DEBUG_BASIC
   printf("[CHAMPSIM_CACHE] ALLOC pc=0x%lx Instruction*=%p with %zu MicroOps (new cache_size=%zu)\n",
          cache_key.pc, (void*)instruction, uops->size(), m_champsim_icache.size());
   #endif
   
   return instruction;
}

Sift::Mode TraceThread::handleInstructionCountFunc(uint32_t icount)
{
   if (!m_started)
   {
      // Received first instruction, let TraceManager know our SIFT connection is up and running
      Sim()->getTraceManager()->signalStarted();
      m_started = true;
   }

   // We may have been blocked in a system call, if we start executing instructions again that means we're continuing
   if (m_blocked)
   {
      unblock();
   }

   Core *core = m_thread->getCore();
   LOG_ASSERT_ERROR(core, "We cannot execute instructions while not on a core");
   SubsecondTime time = core->getPerformanceModel()->getElapsedTime();
   core->countInstructions(0, icount);

   if (Sim()->getInstrumentationMode() == InstMode::DETAILED && icount)
   {
      // We're in detailed mode, but our SIFT recorder doesn't know it yet
      // Do something to advance time
      core->getPerformanceModel()->queuePseudoInstruction(new UnknownInstruction(icount * core->getDvfsDomain()->getPeriod()));
      core->getPerformanceModel()->iterate();
   }

   // We may have been rescheduled
   if (m_thread->reschedule(time, core))
   {
      core = m_thread->getCore();
      core->getPerformanceModel()->queuePseudoInstruction(new SyncInstruction(time, SyncInstruction::UNSCHEDULED));
   }

   switch(Sim()->getInstrumentationMode())
   {
      case InstMode::FAST_FORWARD:
         return Sift::ModeIcount;
      case InstMode::CACHE_ONLY:
         return Sift::ModeMemory;
      case InstMode::DETAILED:
         return Sift::ModeDetailed;
      case InstMode::INVALID:
         return Sift::ModeUnknown;
   }
   assert(false);
}

void TraceThread::handleCacheOnlyFunc(uint8_t icount, Sift::CacheOnlyType type, uint64_t eip, uint64_t address)
{
   Core *core = m_thread->getCore();
   if (!core)
   {
      //LOG_PRINT_WARNING("Ignoring warmup while not on a core");
      return;
   }
   //LOG_ASSERT_ERROR(core, "We cannot perform warmup while not on a core");

   if (icount)
      core->countInstructions(0, icount);

   switch(type)
   {
      case Sift::CacheOnlyBranchTaken:
      case Sift::CacheOnlyBranchNotTaken:
      {
         bool taken = (type == Sift::CacheOnlyBranchTaken);
         bool mispredict = core->accessBranchPredictor(va2pa(eip), taken, false, va2pa(address));
         if (mispredict)
            core->getPerformanceModel()->handleBranchMispredict();
         break;
      }

      case Sift::CacheOnlyMemRead:
      case Sift::CacheOnlyMemWrite:
         core->accessMemory(
               Core::NONE,
               type == Sift::CacheOnlyMemRead ? Core::READ : Core::WRITE,
               va2pa(address),
               NULL,
               4,
               Core::MEM_MODELED_COUNT,
               va2pa(eip));
         break;

      case Sift::CacheOnlyMemIcache:
         if (Sim()->getConfig()->getEnableICacheModeling())
            core->readInstructionMemory(va2pa(eip), address);
         break;
   }
}

const dl::DecodedInst* TraceThread::staticDecode(Sift::Instruction &inst)
{
   dl::DecodedInst *dec_inst = m_factory->CreateInstruction(Sim()->getDecoder(), inst.sinst->data, 
                                                            inst.sinst->size, inst.sinst->addr);
   Sim()->getDecoder()->decode(dec_inst, (dl::dl_isa)inst.isa);
   return dec_inst;
}

void TraceThread::handleInstructionWarmup(Sift::Instruction &inst, Sift::Instruction &next_inst, Core *core, bool do_icache_warmup, UInt64 icache_warmup_addr, UInt64 icache_warmup_size)
{
   if (m_decoder_cache.count(inst.sinst->addr) == 0)
      m_decoder_cache[inst.sinst->addr] = staticDecode(inst);
   
   const dl::DecodedInst &dec_inst = *(m_decoder_cache[inst.sinst->addr]);

   // Warmup instruction caches

   if (do_icache_warmup && Sim()->getConfig()->getEnableICacheModeling())
   {
      core->readInstructionMemory(va2pa(icache_warmup_addr), icache_warmup_size);
   }

   // Warmup branch predictor

   if (inst.is_branch)
   {
      bool mispredict = core->accessBranchPredictor(va2pa(inst.sinst->addr), inst.taken, dec_inst.is_indirect_branch(), va2pa(next_inst.sinst->addr));
      if (mispredict)
         core->getPerformanceModel()->handleBranchMispredict();
   }

   // Warmup data caches

   if (inst.executed)
   {
      const bool is_atomic_update = dec_inst.is_atomic();
      const bool is_prefetch = dec_inst.is_prefetch();

      // Ignore memory-referencing operands in NOP instructions
      if (!dec_inst.is_nop())
      {
         for(uint32_t mem_idx = 0; mem_idx <  Sim()->getDecoder()->num_memory_operands(&dec_inst); ++mem_idx)
         {
            if (Sim()->getDecoder()->op_read_mem(&dec_inst, mem_idx))
            {
               UInt64 mem_address;
               // LDP ARM instructions, second element to be loaded, using the address of the first element
               if (dec_inst.is_mem_pair() && ((int)mem_idx == (inst.num_addresses + 1)))  
               {
                  LOG_ASSERT_ERROR((int)mem_idx < (inst.num_addresses + 1), "Did not receive enough data addresses");
                  
                  mem_address = inst.addresses[mem_idx - 1] + Sim()->getDecoder()->size_mem_op(&dec_inst, mem_idx);
               }
               else
               {
                  LOG_ASSERT_ERROR(mem_idx < inst.num_addresses, "Did not receive enough data addresses");
                 
                  mem_address = inst.addresses[mem_idx];
               }
               
               bool no_mapping = false;
               UInt64 pa = va2pa(mem_address, is_prefetch ? &no_mapping : NULL);
               if (no_mapping)
                  continue;

               core->accessMemory(
                     /*(is_atomic_update) ? Core::LOCK :*/ Core::NONE,
                     (is_atomic_update) ? Core::READ_EX : Core::READ,
                     pa,
                     NULL,
                     Sim()->getDecoder()->size_mem_op(&dec_inst, mem_idx),
                     Core::MEM_MODELED_COUNT,
                     va2pa(inst.sinst->addr));
            }
         }

         for(uint32_t mem_idx = 0; mem_idx < Sim()->getDecoder()->num_memory_operands(&dec_inst); ++mem_idx)
         {
            if (Sim()->getDecoder()->op_write_mem(&dec_inst, mem_idx))
            {
               UInt64 mem_address;
               // STP ARM instructions, second element to be stored, using the address of the first element
               if (dec_inst.is_mem_pair() && ((int)mem_idx == (inst.num_addresses + 1)))  
               {
                  LOG_ASSERT_ERROR((int)mem_idx < (inst.num_addresses + 1), "Did not receive enough data addresses");
                  
                  mem_address = inst.addresses[mem_idx - 1] + Sim()->getDecoder()->size_mem_op(&dec_inst, mem_idx);
               }
               else
               {
                  LOG_ASSERT_ERROR(mem_idx < inst.num_addresses, "Did not receive enough data addresses");
                 
                  mem_address = inst.addresses[mem_idx];
               }
               
               bool no_mapping = false;
               UInt64 pa = va2pa(mem_address, is_prefetch ? &no_mapping : NULL);
               if (no_mapping)
                  continue;

               if (is_atomic_update)
                  core->logMemoryHit(false, Core::WRITE, pa, Core::MEM_MODELED_COUNT, va2pa(inst.sinst->addr));
               else
                  core->accessMemory(
                        /*(is_atomic_update) ? Core::UNLOCK :*/ Core::NONE,
                        Core::WRITE,
                        pa,
                        NULL,
                        Sim()->getDecoder()->size_mem_op(&dec_inst, mem_idx),
                        Core::MEM_MODELED_COUNT,
                        va2pa(inst.sinst->addr));
            }
         }
   }
   }
}

void TraceThread::handleChampSimWarmup(Sift::Instruction &inst, Sift::Instruction &next_inst, Core *core)
{
   // Warmup instruction cache
   if (Sim()->getConfig()->getEnableICacheModeling())
   {
      core->readInstructionMemory(va2pa(inst.sinst->addr), inst.sinst->size);
   }

   // Warmup branch predictor
   if (inst.is_branch)
   {
      bool mispredict = core->accessBranchPredictor(va2pa(inst.sinst->addr), inst.taken, false, va2pa(next_inst.sinst->addr));
      if (mispredict)
         core->getPerformanceModel()->handleBranchMispredict();
   }

   if (!inst.executed)
      return;

   // Warmup data caches
   UInt32 mem_size = m_champsim_access_size ? m_champsim_access_size : Sift::CHAMPSIM_DEFAULT_MEM_SIZE;

   for (uint8_t idx = 0; idx < inst.num_src_addresses; ++idx)
   {
      bool no_mapping = false;
      UInt64 pa = va2pa(inst.src_addresses[idx], &no_mapping);
      if (no_mapping)
         continue;

      core->accessMemory(
            Core::NONE,
            Core::READ,
            pa,
            NULL,
            mem_size,
            Core::MEM_MODELED_COUNT,
            va2pa(inst.sinst->addr));
   }

   for (uint8_t idx = 0; idx < inst.num_dest_addresses; ++idx)
   {
      bool no_mapping = false;
      UInt64 pa = va2pa(inst.dest_addresses[idx], &no_mapping);
      if (no_mapping)
         continue;

      core->accessMemory(
            Core::NONE,
            Core::WRITE,
            pa,
            NULL,
            mem_size,
            Core::MEM_MODELED_COUNT,
            va2pa(inst.sinst->addr));
   }
}

void TraceThread::handleInstructionDetailed(Sift::Instruction &inst, Sift::Instruction &next_inst, PerformanceModel *prfmdl)
{

   // Set up instruction

   if (m_icache.count(inst.sinst->addr) == 0)
      m_icache[inst.sinst->addr] = decode(inst);
   // Here get the decoder instruction without checking, because we must have it for sure
   const dl::DecodedInst &dec_inst = *(m_decoder_cache[inst.sinst->addr]);

   Instruction *ins = m_icache[inst.sinst->addr];
   DynamicInstruction *dynins = prfmdl->createDynamicInstruction(ins, va2pa(inst.sinst->addr));

   // Add dynamic instruction info

   if (inst.is_branch)
   {
      dynins->addBranch(inst.taken, va2pa(next_inst.sinst->addr), dec_inst.is_indirect_branch());
   }

   // Ignore memory-referencing operands in NOP instructions
   if (!dec_inst.is_nop())
   {
      const bool is_prefetch = dec_inst.is_prefetch();

      for(uint32_t mem_idx = 0; mem_idx < Sim()->getDecoder()->num_memory_operands(&dec_inst); ++mem_idx)
      {
         if (Sim()->getDecoder()->op_read_mem(&dec_inst, mem_idx))
         {
            addDetailedMemoryInfo(dynins, inst, dec_inst, mem_idx, Operand::READ, is_prefetch, prfmdl);
         }
      }

      for(uint32_t mem_idx = 0; mem_idx < Sim()->getDecoder()->num_memory_operands(&dec_inst); ++mem_idx)
      {
         if (Sim()->getDecoder()->op_write_mem(&dec_inst, mem_idx))
         {
            addDetailedMemoryInfo(dynins, inst, dec_inst, mem_idx, Operand::WRITE, is_prefetch, prfmdl);
         }
      }
   }


   prfmdl->queueInstruction(dynins);


   prfmdl->iterate();
}

void TraceThread::handleChampSimDetailed(Sift::Instruction &inst, Sift::Instruction &next_inst, PerformanceModel *prfmdl)
{
   #if DEBUG_TRACE_CHAMPSIM >= DEBUG_BASIC
      std::cout << "[TraceThread] Handling ChampSim detailed instruction at address " << std::hex << inst.sinst->addr << std::dec << std::endl;
   #endif 

   // ChampSim instructions are cached by (PC, is_branch, num_src_regs, num_dest_regs, num_loads, num_stores).
   // If the same PC appears with a different operand signature, a new Instruction is created.
   // This avoids unbounded memory growth while handling variable operand counts.
   Instruction *ins = decodeChampsim(inst);
   DynamicInstruction *dynins = prfmdl->createDynamicInstruction(ins, va2pa(inst.sinst->addr));

   #if DEBUG_TRACE_CHAMPSIM >= DEBUG_BASIC
   
      std::ostringstream oss;
      oss << "[TraceThread] ChampSim instruction info: addr=0x" << std::hex << inst.sinst->addr
          << " size=" << std::dec << static_cast<uint32_t>(inst.sinst->size)
          << " branch=" << inst.is_branch
          << " taken=" << inst.taken
          << " predicate=" << inst.is_predicate
          << " executed=" << inst.executed
          << " isa=" << inst.isa
          << " is_champsim=" << inst.is_champsim
          << " num_addresses=" << static_cast<uint32_t>(inst.num_addresses)
          << " addresses=[";
      for (uint8_t idx = 0; idx < inst.num_addresses; ++idx)
      {
         if (idx)
            oss << ',';
         oss << "0x" << std::hex << inst.addresses[idx];
      }
      oss << std::dec << "] src=[";
      for (uint8_t idx = 0; idx < inst.num_src_addresses; ++idx)
      {
         if (idx)
            oss << ',';
         oss << "0x" << std::hex << inst.src_addresses[idx];
      }
      oss << "] dest=[";
      for (uint8_t idx = 0; idx < inst.num_dest_addresses; ++idx)
      {
         if (idx)
            oss << ',';
         oss << "0x" << std::hex << inst.dest_addresses[idx];
      }
      oss << std::dec << "] src_regs=[";
      for (uint8_t idx = 0; idx < inst.num_src_registers; ++idx)
      {
         if (idx)
            oss << ',';
         oss << static_cast<uint32_t>(inst.src_registers[idx]);
      }
      oss << "] dest_regs=[";
      for (uint8_t idx = 0; idx < inst.num_dest_registers; ++idx)
      {
         if (idx)
            oss << ',';
         oss << static_cast<uint32_t>(inst.dest_registers[idx]);
      }
      oss << "]";
      std::cout << oss.str() << std::endl;

      std::ostringstream opss;
      const OperandList &ops = ins->getOperands();
      opss << "[TraceThread] ChampSim operand check: src_regs=[";
      for (uint8_t idx = 0; idx < inst.num_src_registers; ++idx)
      {
         if (idx)
            opss << ',';
         opss << static_cast<uint32_t>(inst.src_registers[idx]);
      }
      opss << "] dest_regs=[";
      for (uint8_t idx = 0; idx < inst.num_dest_registers; ++idx)
      {
         if (idx)
            opss << ',';
         opss << static_cast<uint32_t>(inst.dest_registers[idx]);
      }
      opss << "] operands=[";
      for (size_t i = 0; i < ops.size(); ++i)
      {
         if (i)
            opss << ';';
         opss << ops[i].toString();
      }
      opss << "]";
      std::cout << opss.str() << std::endl;
   
   #endif 

   if (inst.is_branch)
   {
      dynins->addBranch(inst.taken, va2pa(next_inst.sinst->addr), false);
   }

   for (uint8_t idx = 0; idx < inst.num_src_addresses; ++idx)
   {
      addChampSimMemoryInfo(dynins, inst.src_addresses[idx], Operand::READ, inst.executed, false, prfmdl);
   }

   for (uint8_t idx = 0; idx < inst.num_dest_addresses; ++idx)
   {
      addChampSimMemoryInfo(dynins, inst.dest_addresses[idx], Operand::WRITE, inst.executed, false, prfmdl);
   }

   prfmdl->queueInstruction(dynins);
   prfmdl->iterate();
}

void TraceThread::addDetailedMemoryInfo(DynamicInstruction *dynins, Sift::Instruction &inst, const dl::DecodedInst &decoded_inst, uint32_t mem_idx, Operand::Direction op_type, bool is_prefetch, PerformanceModel *prfmdl)
{
   UInt64 mem_address;
   // LDP/STP ARM instructions, second element to be ld/st, using the address of the first element
   if (decoded_inst.is_mem_pair() && ((int)mem_idx == inst.num_addresses))  
   {
      assert((int)mem_idx < (inst.num_addresses + 1));
      mem_address = inst.addresses[mem_idx - 1] + Sim()->getDecoder()->size_mem_op(&decoded_inst, mem_idx);
   }
   else
   {
      assert(mem_idx < inst.num_addresses);
      mem_address = inst.addresses[mem_idx];
   }
               
   bool no_mapping = false;
   UInt64 pa = va2pa(mem_address, is_prefetch ? &no_mapping : NULL);
   

   if (no_mapping)
   {
      dynins->addMemory(
         inst.executed,
         SubsecondTime::Zero(),
         0,
         Sim()->getDecoder()->size_mem_op(&decoded_inst, mem_idx),
         op_type,
         0,
         HitWhere::PREFETCH_NO_MAPPING);
   }
   else
   {
      dynins->addMemory(
         inst.executed,
         SubsecondTime::Zero(),
         pa,
         Sim()->getDecoder()->size_mem_op(&decoded_inst, mem_idx),
         op_type,
         0,
         HitWhere::UNKNOWN);
   }
}

void TraceThread::addChampSimMemoryInfo(DynamicInstruction *dynins, UInt64 mem_address, Operand::Direction op_type, bool executed, bool is_prefetch, PerformanceModel *prfmdl)
{
   bool no_mapping = false;
   UInt64 pa = va2pa(mem_address, is_prefetch ? &no_mapping : NULL);
   UInt32 mem_size = m_champsim_access_size ? m_champsim_access_size : Sift::CHAMPSIM_DEFAULT_MEM_SIZE;

   if (no_mapping)
   {
      dynins->addMemory(
         executed,
         SubsecondTime::Zero(),
         0,
         mem_size,
         op_type,
         0,
         HitWhere::PREFETCH_NO_MAPPING);
   }
   else
   {
      dynins->addMemory(
         executed,
         SubsecondTime::Zero(),
         pa,
         mem_size,
         op_type,
         0,
         HitWhere::UNKNOWN);
   }
}

void TraceThread::unblock()
{
   LOG_ASSERT_ERROR(m_blocked == true, "Must call only when m_blocked == true");

   SubsecondTime end_time = Sim()->getClockSkewMinimizationServer()->getGlobalTime(true /*upper_bound*/);
   m_thread->getSyscallMdl()->runExit(0);
   {
      ScopedLock sl(Sim()->getThreadManager()->getLock());
      // We were blocked on a system call, but started executing instructions again.
      // This means we woke up. Since there is no explicit wakeup nor an associated time,
      // use global time.
      Sim()->getThreadManager()->resumeThread_async(m_thread->getId(), INVALID_THREAD_ID, end_time, NULL);
   }

   if (m_thread->getCore())
   {
      m_thread->getCore()->getPerformanceModel()->queuePseudoInstruction(new SyncInstruction(end_time, SyncInstruction::SYSCALL));
   }
   else
   {
      // We were scheduled out during the system call
      m_thread->reschedule(end_time, NULL);
      m_thread->getCore()->getPerformanceModel()->queuePseudoInstruction(new SyncInstruction(end_time, SyncInstruction::UNSCHEDULED));
   }

   m_blocked = false;
}

void TraceThread::m_run_func_with_userpace_mimicos()
{
   // Set thread name for Sniper-in-Sniper simulations
   String threadName = String("trace-") + itostr(m_thread->getId());
   SimSetThreadName(threadName.c_str());

   // Assign this worker a CPU from the startup affinity mask.
   pinTraceThreadToCPU(m_thread->getId());

   // Print which host CPU this thread is running on
   int host_cpu = sched_getcpu();
   std::cout << "[TraceThread] Thread " << m_thread->getId()
             << " (app " << m_app_id << ") started on host CPU " << host_cpu << std::endl;

#if DEBUG_TRACE_THREAD >= DEBUG_DETAILED
   std::cout << "[TRACE:" << m_thread->getId() << "] -- " << threadName.c_str() << " --" << std::endl;
#endif
   Sim()->getThreadManager()->onThreadStart(m_thread->getId(), m_time_start);

   // Open the trace (be sure to do this before potentially blocking on reschedule() as this causes deadlock)
   m_trace.initStream();
   m_trace_has_pa = m_trace.getTraceHasPhysicalAddresses();
   m_champsim_trace = m_trace.isChampSimTrace();

   if (m_thread->getCore() == NULL)
   {
#if DEBUG_TRACE_THREAD >= DEBUG_DETAILED
      std::cout << "[TRACE:" << m_thread->getId() << "] -- Waiting for core to be assigned --" << std::endl;
#endif
      // We didn't get scheduled on startup, wait here
      SubsecondTime time = SubsecondTime::Zero();
      m_thread->reschedule(time, NULL);
   }

   Core *core = m_thread->getCore();
   PerformanceModel *prfmdl = core->getPerformanceModel();

   Sift::Instruction inst, next_inst;
   Sift::Instruction inst_kernel, next_inst_kernel;
   Sift::Instruction inst_app, next_inst_app;

   // Initialise instruction state
   to_be_replayed_inst.sinst = NULL;
   to_be_replayed_next_inst.sinst = NULL;
   inst_app.sinst = next_inst_app.sinst = NULL; // App reader state

   bool app_initialized = false;

   SubsecondTime kernel_start_time = SubsecondTime::Zero();
   SubsecondTime kernel_end_time = SubsecondTime::Zero();
   // By design, kernel SIFT reader runs first
   bool have_inst = m_current_sift_reader->Read(inst_kernel);


   kernel_start_time = prfmdl->getElapsedTime();

   if (have_inst)
   {
      inst = inst_kernel;

      int  executed_instructions = 0;
#if DEBUG_TRACE_THREAD >= DEBUG_DETAILED
      bool kernel_mode           = true;
      bool switched              = false;
#else
      bool kernel_mode           __attribute__((unused)) = true;
      bool switched              __attribute__((unused)) = false;
#endif

      while (have_inst)
      {
         bool have_next = false;
         switched = false;

#if DEBUG_TRACE_THREAD >= DEBUG_DETAILED
         std::cout << "[TRACE:" << m_thread->getId() << "] -- Reading instruction: "
                   << std::hex << inst.sinst->addr << std::dec
                   << " from trace_id = " << getCurrentSiftReader()->getId() << std::endl;
#endif

         if (getCurrentSiftReader() != getAppSiftReader())
         {
            // Currently reading from kernel SIFT reader

            have_next = m_current_sift_reader->Read(next_inst_kernel);
            if (!have_next)
               break;   // EOF on kernel trace

            next_inst = next_inst_kernel;

            // NOTE: if we consumed a Context Switch command here, magic_server.cc
            // may just have switched currentSiftReader: Kernel -> App.
            if (getCurrentSiftReader() == getAppSiftReader())
            {

               switched = true;
#if DEBUG_TRACE_THREAD >= DEBUG_DETAILED
               std::cout << "[TRACE:" << m_thread->getId() << "] -- Switching SIFT reader to the App due to CS --" << std::endl;
#endif
               // Finished processing kernel instructions; record end time
               kernel_end_time = prfmdl->getElapsedTime();
               stats.kernel_time += (kernel_end_time - kernel_start_time);
               kernel_mode = false;

               if (to_be_replayed_inst.sinst)
               {
                  // Returning from a page fault: restore exact app state
                  assert(to_be_replayed_inst.sinst == inst_app.sinst &&
                         to_be_replayed_next_inst.sinst == next_inst_app.sinst);

                  inst      = to_be_replayed_inst;
                  next_inst = to_be_replayed_next_inst;
                  to_be_replayed_inst.sinst = to_be_replayed_next_inst.sinst = NULL;
                  have_next = true; // next_inst is valid by construction

#if DEBUG_TRACE_THREAD >= DEBUG_DETAILED
                  std::cout << "[TRACE:" << m_thread->getId() << "] -- Replaying instruction: inst = "
                            << std::hex << inst.sinst->addr << std::dec << std::endl;
#endif
               }
               else
               {
                  // First time or normal resume on app reader
                  if (!app_initialized)
                  {
                     // First time: read both inst_app and next_inst_app
                     if (!m_current_sift_reader->Read(inst_app))
                        break;
#if DEBUG_TRACE_THREAD >= DEBUG_DETAILED
                     std::cout << "[TRACE:" << m_thread->getId() << "] -- First instruction from App SIFT reader: inst = "
                               << std::hex << inst_app.sinst->addr << std::dec << std::endl;
#endif
                     if (!m_current_sift_reader->Read(next_inst_app))
                        break;
                     app_initialized = true;
                  }
                  else
                  {
                     // Continue: shift lookahead then read a new next_inst_app
                     inst_app = next_inst_app;
                     if (!m_current_sift_reader->Read(next_inst_app))
                        break;
#if DEBUG_TRACE_THREAD >= DEBUG_DETAILED
                     std::cout << "[TRACE:" << m_thread->getId() << "] -- Restored instruction from App SIFT reader: inst = "
                               << std::hex << inst_app.sinst->addr << std::dec << std::endl;
#endif
                  }

                  inst      = inst_app;
                  next_inst = next_inst_app;
                  have_next = true;
               }

#if DEBUG_TRACE_THREAD >= DEBUG_DETAILED
               std::cout << "[TRACE:" << m_thread->getId() << "] -- Instruction: " << std::hex << inst.sinst->addr << std::dec
                         << " -- Next instruction: " << std::hex << next_inst.sinst->addr << std::dec
                         << " -- Kernel mode: " << (kernel_mode ? "true" : "false") << std::endl;
#endif
            }
         }
         else
         {
            // Currently reading from app SIFT reader

            // We assume app_initialized is true when we get here
            inst_app = next_inst_app;
            if (!m_current_sift_reader->Read(next_inst_app))
               break;   // EOF on app trace

            next_inst = next_inst_app;
            have_next = true;
         }

         if (!m_started)
         {
            // Received first instructions, let TraceManager know our SIFT connection is up and running
            // Only enable once we have received two instructions, otherwise, we could deadlock
            Sim()->getTraceManager()->signalStarted();
            m_started = true;
         }

         // We may have been blocked in a system call; starting to execute again means we continue
         if (m_blocked)
         {
            unblock();
         }

         core = m_thread->getCore();
         LOG_ASSERT_ERROR(core, "We cannot execute instructions while not on a core");
         prfmdl = core->getPerformanceModel();

         bool   do_icache_warmup = false;
         UInt64 icache_warmup_addr = 0, icache_warmup_size = 0;

         // Reconstruct and count basic blocks

         if (m_bbv_end || m_bbv_last != inst.sinst->addr)
         {
            // We're the start of a new basic block
            core->countInstructions(m_bbv_base, m_bbv_count);
            // In cache-only mode, we'll want to do I-cache warmup
            if (m_bbv_base)
            {
               do_icache_warmup = true;
               icache_warmup_addr = m_bbv_base;
               icache_warmup_size = m_bbv_last - m_bbv_base;
            }
            // Set up new basic block info
            m_bbv_base = inst.sinst->addr;
            m_bbv_count = 0;
         }
         m_bbv_count++;
         m_bbv_last = inst.sinst->addr + inst.sinst->size;
         // Force BBV end on non-taken branches
         m_bbv_end = inst.is_branch;

         // Kernel-mode (MimicOS) instructions are always fast-forwarded:
         // we only model the application's performance in detail.
         bool in_kernel_mode = (getCurrentSiftReader() != getAppSiftReader());

         if (in_kernel_mode)
         {
            // Fast-forward: just count the instruction
            core->countInstructions(0, 1);
         }
         else if (inst.is_champsim)
         {
            switch(Sim()->getInstrumentationMode())
            {
               case InstMode::FAST_FORWARD:
                  break;

               case InstMode::CACHE_ONLY:
                  handleChampSimWarmup(inst, next_inst, core);
                  break;

               case InstMode::DETAILED:
                  handleChampSimDetailed(inst, next_inst, prfmdl);
                  break;

               default:
                  LOG_PRINT_ERROR("Unknown instrumentation mode");
            }
         }
         else
         {
            switch(Sim()->getInstrumentationMode())
            {
               case InstMode::FAST_FORWARD:
                  break;

               case InstMode::CACHE_ONLY:
                  handleInstructionWarmup(inst, next_inst, core, do_icache_warmup, icache_warmup_addr, icache_warmup_size);
                  break;

               case InstMode::DETAILED:
                  handleInstructionDetailed(inst, next_inst, prfmdl);
                  break;

               default:
                  LOG_PRINT_ERROR("Unknown instrumentation mode");
            }
         }

         // We may have been rescheduled to a different core
         // by prfmdl->iterate (in handleInstructionDetailed),
         // or core->countInstructions (when using a fast-forward performance model)
         {
            SubsecondTime time = prfmdl->getElapsedTime();
            if (m_thread->reschedule(time, core))
            {
               core = m_thread->getCore();
               prfmdl = core->getPerformanceModel();
            }
         }

         if (m_stop)
         {
#if DEBUG_TRACE_THREAD >= DEBUG_DETAILED
            std::cout << "[TRACE:" << m_thread->getId() << "] -- Stopping trace thread --" << std::endl;
#endif
            break;
         }

         auto* mimic_os = Sim()->getMimicOS();
         core_id_t pf_core_id = core->getId();

         if (mimic_os->getIsPageFault(pf_core_id))
         {
            kernel_start_time = prfmdl->getElapsedTime();
            // Page fault must occur only in user space (app reader active)
            assert(getCurrentSiftReader() == getAppSiftReader());
#if DEBUG_TRACE_THREAD >= DEBUG_DETAILED
            std::cout << "[TRACE:" << m_thread->getId() << "] -- Switching SIFT reader back to the kernel due to page fault CS --" << std::endl;
#endif

            // Save app instruction pair to replay after kernel handles the fault
            to_be_replayed_inst      = inst_app;
            to_be_replayed_next_inst = next_inst_app;

            int num_requested_frames = mimic_os->getNumRequestedFrames(pf_core_id);
#if DEBUG_TRACE_THREAD >= DEBUG_BASIC
            std::cout << "[TRACE:" << m_thread->getId() << "] -- Requesting "
                      << num_requested_frames << " frames for page fault handling" << std::endl;
#endif

            mimic_os->buildMessageWithArgs("page_fault",
                                           mimic_os->getVaTriggeredPageFault(pf_core_id),
                                           num_requested_frames);

            // Handle PF on user-space MimicOS (kernel) side
#if DEBUG_TRACE_THREAD >= DEBUG_DETAILED
            std::cout << "[TRACE:" << m_thread->getId() << "] -- Switching SIFT reader to Kernel and sending response after CS --" << std::endl;
#endif
            setCurrentSiftReader(getKernelSiftReader());

            // Unlock Sift::Writer on MimicOS (kernel) side
            getCurrentSiftReader()->sendResponseAfterContextSwitch();

            // First instruction after switching back to kernel
            bool have_next_pf = m_current_sift_reader->Read(next_inst_kernel);
            if (!have_next_pf)
               break;   // EOF on kernel after PF handling

            next_inst = next_inst_kernel;
            have_next = have_next_pf;

            kernel_mode = true;
            mimic_os->resetPageFaultState(pf_core_id);
         }

         // Save current "next_inst" as the next "inst"
         inst      = next_inst;
         have_inst = have_next;

#ifdef HEARTBEAT
         if (executed_instructions % 20000 == 0)
         {
            // Print progress every 20k instructions
            std::cout << "[TRACE:" << m_thread->getId() << "] -- Current SIFT reader: "
                      << (getCurrentSiftReader() == getKernelSiftReader() ? "Kernel" : "App") << " -- "
                      << "Executed instructions: " << executed_instructions
                      << ", Current time: " << prfmdl->getElapsedTime().getNS() << "ns"
                      << ", Current SIFT reader position: " << m_current_sift_reader->getPosition()
                      << std::endl;
            std::cout << "[TRACE:" << m_thread->getId() << "] -- Current instruction: " << std::hex << inst.sinst->addr << std::dec
                      << ", Next instruction: " << std::hex << next_inst.sinst->addr << std::dec
                      << ", Kernel mode: " << (kernel_mode ? "true" : "false")
                      << std::endl;
         }
#endif

         executed_instructions++;
      }
   }

   printf("[TRACE:%u] -- %s --\n", m_thread->getId(), m_stop ? "STOP" : "DONE");
   
   // Clean up ChampSim instruction cache to avoid leaks (destructor is never called)
   if (m_champsim_trace) {
      cleanupChampSimCache();
   }

   SubsecondTime time_end = prfmdl->getElapsedTime();

   Sim()->getThreadManager()->onThreadExit(m_thread->getId());
   Sim()->getTraceManager()->signalDone(this, time_end, m_stop /*aborted*/);
}



void TraceThread::m_run_func_default()
{
   // Set thread name for Sniper-in-Sniper simulations
   String threadName = String("trace-") + itostr(m_thread->getId());
   SimSetThreadName(threadName.c_str());

   // Assign this worker a CPU from the startup affinity mask.
   pinTraceThreadToCPU(m_thread->getId());

   // Print which host CPU this thread is running on
   int host_cpu = sched_getcpu();
   std::cout << "[TraceThread] Thread " << m_thread->getId()
             << " (app " << m_app_id << ") started on host CPU " << host_cpu << std::endl;

   Sim()->getThreadManager()->onThreadStart(m_thread->getId(), m_time_start);

   // Open the trace (be sure to do this before potentially blocking on reschedule() as this causes deadlock)
   m_trace.initStream();
   m_trace_has_pa = m_trace.getTraceHasPhysicalAddresses();
   m_champsim_trace = m_trace.isChampSimTrace();

   if (m_thread->getCore() == NULL)
   {
      // We didn't get scheduled on startup, wait here
      SubsecondTime time = SubsecondTime::Zero();
      m_thread->reschedule(time, NULL);
   }

   Core *core = m_thread->getCore();
   PerformanceModel *prfmdl = core->getPerformanceModel();

   Sift::Instruction inst, next_inst;

   bool have_first = m_trace.Read(inst);

   while(have_first && m_trace.Read(next_inst))
   {
   
      if (!m_started)
      {
         // Received first instructions, let TraceManager know our SIFT connection is up and running
         // Only enable once we have received two instructions, otherwise, we could deadlock
         Sim()->getTraceManager()->signalStarted();
         m_started = true;
      }
      if (m_blocked)
      {
         unblock();
      }

      core = m_thread->getCore();
      prfmdl = core->getPerformanceModel();

      bool do_icache_warmup = false;
      UInt64 icache_warmup_addr = 0, icache_warmup_size = 0;

      // Reconstruct and count basic blocks

      if (m_bbv_end || m_bbv_last != inst.sinst->addr)
      {
         // We're the start of a new basic block
         core->countInstructions(m_bbv_base, m_bbv_count);
         // In cache-only mode, we'll want to do I-cache warmup
         if (m_bbv_base)
         {
            do_icache_warmup = true;
            icache_warmup_addr = m_bbv_base;
            icache_warmup_size = m_bbv_last - m_bbv_base;
         }
         // Set up new basic block info
         m_bbv_base = inst.sinst->addr;
         m_bbv_count = 0;
      }
      m_bbv_count++;
      m_bbv_last = inst.sinst->addr + inst.sinst->size;
      // Force BBV end on non-taken branches
      m_bbv_end = inst.is_branch;


      if (inst.is_champsim)
      {
         switch(Sim()->getInstrumentationMode())
         {
            case InstMode::FAST_FORWARD:
               break;

            case InstMode::CACHE_ONLY:
               handleChampSimWarmup(inst, next_inst, core);
               break;

            case InstMode::DETAILED:
               handleChampSimDetailed(inst, next_inst, prfmdl);
               break;

            default:
               LOG_PRINT_ERROR("Unknown instrumentation mode");
         }
      }
      else
      {
         switch(Sim()->getInstrumentationMode())
         {
            case InstMode::FAST_FORWARD:
               break;

            case InstMode::CACHE_ONLY:
               handleInstructionWarmup(inst, next_inst, core, do_icache_warmup, icache_warmup_addr, icache_warmup_size);
               break;

            case InstMode::DETAILED:
               handleInstructionDetailed(inst, next_inst, prfmdl);
               break;

            default:
               LOG_PRINT_ERROR("Unknown instrumentation mode");
         }
      }


      // We may have been rescheduled to a different core
      // by prfmdl->iterate (in handleInstructionDetailed),
      // or core->countInstructions (when using a fast-forward performance model)
      SubsecondTime time = prfmdl->getElapsedTime();
      if (m_thread->reschedule(time, core))
      {
         core = m_thread->getCore();
         prfmdl = core->getPerformanceModel();
      }


      if (m_stop)
         break;

      inst = next_inst;
   }

   printf("[TRACE:%u] -- %s --\n", m_thread->getId(), m_stop ? "STOP" : "DONE");
   
   // Clean up ChampSim instruction cache to avoid leaks (destructor is never called)
   if (m_champsim_trace) {
      cleanupChampSimCache();
   }

   SubsecondTime time_end = prfmdl->getElapsedTime();

   Sim()->getThreadManager()->onThreadExit(m_thread->getId());
   Sim()->getTraceManager()->signalDone(this, time_end, m_stop /*aborted*/);
}

void TraceThread::spawn()
{
   m__thread = _Thread::create(this);
   m__thread->run();
}

UInt64 TraceThread::getProgressExpect()
{
   Sift::Reader *reader = m_current_sift_reader ? m_current_sift_reader : &m_trace;
   return reader->getLength();
}

UInt64 TraceThread::getProgressValue()
{
   Sift::Reader *reader = m_current_sift_reader ? m_current_sift_reader : &m_trace;
   return reader->getPosition();
}

void TraceThread::frontEndStop(){
   // Guard against being called before thread is fully initialized or after it's stopped
   if (m_stopped)
   {
      return;
   }

   if (m_current_sift_reader != NULL)
   {
      m_current_sift_reader->frontEndStop();
   }
   else
   {
      m_trace.frontEndStop();
   }
}

void TraceThread::handleAccessMemory(Core::lock_signal_t lock_signal, Core::mem_op_t mem_op_type, IntPtr d_addr, char* data_buffer, UInt32 data_size)
{
   Sift::MemoryLockType sift_lock_signal;
   Sift::MemoryOpType sift_mem_op;

   switch (lock_signal)
   {
      case (Core::NONE):
         sift_lock_signal = Sift::MemNoLock;
         break;
      case (Core::LOCK):
         sift_lock_signal = Sift::MemLock;
         break;
      case (Core::UNLOCK):
         sift_lock_signal = Sift::MemUnlock;
         break;
      default:
         sift_lock_signal = Sift::MemInvalidLock;
         break;
   }

   switch (mem_op_type)
   {
      case (Core::READ):
      case (Core::READ_EX):
         sift_mem_op = Sift::MemRead;
         break;
      case (Core::WRITE):
         sift_mem_op = Sift::MemWrite;
         break;
      default:
         sift_mem_op = Sift::MemInvalidOp;
         break;
   }

   Sift::Reader *reader = m_current_sift_reader ? m_current_sift_reader : &m_trace;
   reader->AccessMemory(sift_lock_signal, sift_mem_op, d_addr, (uint8_t*)data_buffer, data_size);
}
