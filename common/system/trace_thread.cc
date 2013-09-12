#include "trace_thread.h"
#include "trace_manager.h"
#include "simulator.h"
#include "core_manager.h"
#include "thread_manager.h"
#include "thread.h"
#include "dvfs_manager.h"
#include "instruction.h"
#include "basic_block.h"
#include "performance_model.h"
#include "instruction_decoder.h"
#include "config.hpp"
#include "syscall_model.h"
#include "core.h"
#include "magic_client.h"
#include "branch_predictor.h"
#include "rng.h"
#include "routine_tracer.h"

#include <unistd.h>
#include <sys/syscall.h>

TraceThread::TraceThread(Thread *thread, SubsecondTime time_start, String tracefile, String responsefile, app_id_t app_id, bool cleanup)
   : m__thread(NULL)
   , m_thread(thread)
   , m_time_start(time_start)
   , m_trace(tracefile.c_str(), responsefile.c_str(), thread->getId())
   , m_trace_has_pa(false)
   , m_address_randomization(Sim()->getCfg()->getBool("traceinput/address_randomization"))
   , m_stop(false)
   , m_bbv_base(0)
   , m_bbv_count(0)
   , m_bbv_last(0)
   , m_bbv_end(false)
   , m_output_leftover_size(0)
   , m_tracefile(tracefile)
   , m_responsefile(responsefile)
   , m_app_id(app_id)
   , m_cleanup(cleanup)
   , m_stopped(false)
{
   if (Sim()->getCfg()->getBool("traceinput/mirror_output"))
      m_trace.setHandleOutputFunc(TraceThread::__handleOutputFunc, this);
   m_trace.setHandleSyscallFunc(TraceThread::__handleSyscallFunc, this);
   m_trace.setHandleNewThreadFunc(TraceThread::__handleNewThreadFunc, this);
   m_trace.setHandleJoinFunc(TraceThread::__handleJoinFunc, this);
   m_trace.setHandleMagicFunc(TraceThread::__handleMagicFunc, this);
   m_trace.setHandleEmuFunc(TraceThread::__handleEmuFunc, this);
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

   String syntax = Sim()->getCfg()->getString("general/syntax");
   if (syntax == "intel")
      m_syntax = XED_SYNTAX_INTEL;
   else if (syntax == "att")
      m_syntax = XED_SYNTAX_ATT;
   else if (syntax == "xed")
      m_syntax = XED_SYNTAX_XED;
   else
      LOG_PRINT_ERROR("Unknown assembly syntax %s, should be intel, att or xed.", syntax.c_str());
}

TraceThread::~TraceThread()
{
   delete m__thread;
   if (m_cleanup)
   {
      unlink(m_tracefile.c_str());
      unlink(m_responsefile.c_str());
   }
}

UInt64 TraceThread::va2pa(UInt64 va)
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
         LOG_PRINT_WARNING("No mapping found for logical address %lx", va);
         // Fall through to construct an address with our thread id in the upper bits (assume address is private)
      }
   }

   if (m_address_randomization)
   {
      // Set 16 bits to app_id | remap middle 36 bits using app_id-specific mapping | keep lower 12 bits (page offset)
      return (UInt64(m_thread->getAppId()) << pa_core_shift) | (remapAddress(va >> va_page_shift) << va_page_shift) | (va & va_page_mask);
   }
   else
   {
      // Set 16 bits to app_id | keep lower 48 bits
      return (UInt64(m_thread->getAppId()) << pa_core_shift) | (va & pa_va_mask);
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

         m_thread->getSyscallMdl()->runEnter(syscall_number, *args);
         ret = m_thread->getSyscallMdl()->runExit(ret);
         break;
      }
   }

   return ret;
}

int32_t TraceThread::handleNewThreadFunc()
{
   return Sim()->getTraceManager()->createThread(m_app_id, getCurrentTime(), m_thread->getId());
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

void TraceThread::handleRoutineChangeFunc(int64_t eip, int64_t esp, Sift::RoutineOpType event)
{
   switch(event)
   {
      case Sift::RoutineEnter:
         m_thread->getRoutineTracer()->routineEnter(eip, esp);
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
      default:
         // Not emulated
         return false;
   }
}

void TraceThread::handleRoutineAnnounceFunc(int64_t eip, const char *name, const char *imgname, uint64_t offset, uint32_t line, uint32_t column, const char *filename)
{
   Sim()->getRoutineTracer()->addRoutine(eip, name, imgname, offset, column, line, filename);
}

SubsecondTime TraceThread::getCurrentTime() const
{
   LOG_ASSERT_ERROR(m_thread->getCore() != NULL, "Cannot get time while not on a core");
   return m_thread->getCore()->getPerformanceModel()->getElapsedTime();
}

BasicBlock* TraceThread::decode(Sift::Instruction &inst)
{
   const xed_decoded_inst_t &xed_inst = inst.sinst->xed_inst;

   OperandList list;

   // Ignore memory-referencing operands in NOP instructions
   if (!xed_decoded_inst_get_attribute(&xed_inst, XED_ATTRIBUTE_NOP))
   {
      for(uint32_t mem_idx = 0; mem_idx < xed_decoded_inst_number_of_memory_operands(&xed_inst); ++mem_idx)
         if (xed_decoded_inst_mem_read(&xed_inst, mem_idx))
            list.push_back(Operand(Operand::MEMORY, 0, Operand::READ));

      for(uint32_t mem_idx = 0; mem_idx < xed_decoded_inst_number_of_memory_operands(&xed_inst); ++mem_idx)
         if (xed_decoded_inst_mem_written(&xed_inst, mem_idx))
            list.push_back(Operand(Operand::MEMORY, 0, Operand::WRITE));
   }

   Instruction *instruction;
   if (inst.is_branch)
      instruction = new BranchInstruction(list);
   else
      instruction = new GenericInstruction(list);

   instruction->setAddress(va2pa(inst.sinst->addr));
   instruction->setSize(inst.sinst->size);
   instruction->setAtomic(xed_operand_values_get_atomic(xed_decoded_inst_operands_const(&xed_inst)));
   char disassembly[64];
   xed_format(m_syntax, &xed_inst, disassembly, sizeof(disassembly) - 1, inst.sinst->addr);
   instruction->setDisassembly(disassembly);

   const std::vector<const MicroOp*> *uops = InstructionDecoder::decode(inst.sinst->addr, &xed_inst, instruction);
   instruction->setMicroOps(uops);

   BasicBlock *basic_block = new BasicBlock();
   basic_block->push_back(instruction);

   return basic_block;
}

void TraceThread::handleInstructionWarmup(Sift::Instruction &inst, Sift::Instruction &next_inst, Core *core, bool do_icache_warmup, UInt64 icache_warmup_addr, UInt64 icache_warmup_size)
{
   const xed_decoded_inst_t &xed_inst = inst.sinst->xed_inst;

   // Warmup instruction caches

   if (do_icache_warmup && Sim()->getConfig()->getEnableICacheModeling())
   {
      core->readInstructionMemory(va2pa(icache_warmup_addr), icache_warmup_size);
   }

   // Warmup branch predictor

   if (inst.is_branch)
   {
      PerformanceModel *prfmdl = core->getPerformanceModel();
      BranchPredictor *bp = prfmdl->getBranchPredictor();

      if (bp)
      {
         bool prediction = bp->predict(va2pa(inst.sinst->addr), va2pa(next_inst.sinst->addr));
         bp->update(prediction, inst.taken, va2pa(inst.sinst->addr), va2pa(next_inst.sinst->addr));
      }
   }

   // Warmup data caches

   if (inst.executed)
   {
      const bool is_atomic_update = xed_operand_values_get_atomic(xed_decoded_inst_operands_const(&xed_inst));

      // Ignore memory-referencing operands in NOP instructions
      if (!xed_decoded_inst_get_attribute(&xed_inst, XED_ATTRIBUTE_NOP))
      {
         for(uint32_t mem_idx = 0; mem_idx < xed_decoded_inst_number_of_memory_operands(&xed_inst); ++mem_idx)
         {
            if (xed_decoded_inst_mem_read(&xed_inst, mem_idx))
            {
               LOG_ASSERT_ERROR(mem_idx < inst.num_addresses, "Did not receive enough data addresses");
               core->accessMemory(
                     /*(is_atomic_update) ? Core::LOCK :*/ Core::NONE,
                     (is_atomic_update) ? Core::READ_EX : Core::READ,
                     va2pa(inst.addresses[mem_idx]),
                     NULL,
                     xed_decoded_inst_get_memory_operand_length(&xed_inst, mem_idx),
                     Core::MEM_MODELED_COUNT,
                     va2pa(inst.sinst->addr));
            }
         }

         for(uint32_t mem_idx = 0; mem_idx < xed_decoded_inst_number_of_memory_operands(&xed_inst); ++mem_idx)
         {
            if (xed_decoded_inst_mem_written(&xed_inst, mem_idx))
            {
               LOG_ASSERT_ERROR(mem_idx < inst.num_addresses, "Did not receive enough data addresses");
               if (is_atomic_update)
                  core->logMemoryHit(false, Core::WRITE, va2pa(inst.addresses[mem_idx]), Core::MEM_MODELED_COUNT, va2pa(inst.sinst->addr));
               else
                  core->accessMemory(
                        /*(is_atomic_update) ? Core::UNLOCK :*/ Core::NONE,
                        Core::WRITE,
                        va2pa(inst.addresses[mem_idx]),
                        NULL,
                        xed_decoded_inst_get_memory_operand_length(&xed_inst, mem_idx),
                        Core::MEM_MODELED_COUNT,
                        va2pa(inst.sinst->addr));
            }
         }
      }
   }
}

void TraceThread::handleInstructionDetailed(Sift::Instruction &inst, Sift::Instruction &next_inst, PerformanceModel *prfmdl)
{
   const xed_decoded_inst_t &xed_inst = inst.sinst->xed_inst;

   // Push basic block containing this instruction

   if (m_icache.count(inst.sinst->addr) == 0)
      m_icache[inst.sinst->addr] = decode(inst);
   BasicBlock *basic_block = m_icache[inst.sinst->addr];

   prfmdl->queueBasicBlock(basic_block);

   // Push dynamic instruction info

   if (inst.is_branch)
   {
      DynamicInstructionInfo info = DynamicInstructionInfo::createBranchInfo(va2pa(inst.sinst->addr), inst.taken, va2pa(next_inst.sinst->addr));
      prfmdl->pushDynamicInstructionInfo(info);
   }

   // Ignore memory-referencing operands in NOP instructions
   if (!xed_decoded_inst_get_attribute(&xed_inst, XED_ATTRIBUTE_NOP))
   {
      for(uint32_t mem_idx = 0; mem_idx < xed_decoded_inst_number_of_memory_operands(&xed_inst); ++mem_idx)
      {
         if (xed_decoded_inst_mem_read(&xed_inst, mem_idx))
         {
            assert(mem_idx < inst.num_addresses);
            DynamicInstructionInfo info = DynamicInstructionInfo::createMemoryInfo(va2pa(inst.sinst->addr), inst.executed, SubsecondTime::Zero(), va2pa(inst.addresses[mem_idx]), xed_decoded_inst_get_memory_operand_length(&xed_inst, mem_idx), Operand::READ, 0, HitWhere::UNKNOWN);
            prfmdl->pushDynamicInstructionInfo(info);
         }
      }

      for(uint32_t mem_idx = 0; mem_idx < xed_decoded_inst_number_of_memory_operands(&xed_inst); ++mem_idx)
      {
         if (xed_decoded_inst_mem_written(&xed_inst, mem_idx))
         {
            assert(mem_idx < inst.num_addresses);
            DynamicInstructionInfo info = DynamicInstructionInfo::createMemoryInfo(va2pa(inst.sinst->addr), inst.executed, SubsecondTime::Zero(), va2pa(inst.addresses[mem_idx]), xed_decoded_inst_get_memory_operand_length(&xed_inst, mem_idx), Operand::WRITE, 0, HitWhere::UNKNOWN);
            prfmdl->pushDynamicInstructionInfo(info);
         }
      }
   }

   // simulate

   prfmdl->iterate();

   SubsecondTime time = prfmdl->getElapsedTime();
   m_thread->reschedule(time, NULL);
}

void TraceThread::run()
{
   Sim()->getThreadManager()->onThreadStart(m_thread->getId(), m_time_start);

   // Open the trace (be sure to do this before potentially blocking on reschedule() as this causes deadlock)
   m_trace.initStream();
   m_trace_has_pa = m_trace.getTraceHasPhysicalAddresses();

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
   // Recieved first instruction, let TraceManager know our SIFT connection is up and running
   Sim()->getTraceManager()->signalStarted();

   while(have_first && m_trace.Read(next_inst))
   {
      // We may have been rescheduled to a different core
      // by prfmdl->iterate (at the end of the last iteration),
      // or a system call (handled out-of-band by m_trace.Read)
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


      if (m_stop)
         break;

      inst = next_inst;
   }

   printf("[TRACE:%u] -- %s --\n", m_thread->getId(), m_stop ? "STOP" : "DONE");

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
   return m_trace.getLength();
}

UInt64 TraceThread::getProgressValue()
{
   return m_trace.getPosition();
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

   m_trace.AccessMemory(sift_lock_signal, sift_mem_op, d_addr, (uint8_t*)data_buffer, data_size);
}
