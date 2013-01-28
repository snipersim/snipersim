#include "trace_thread.h"
#include "trace_manager.h"
#include "simulator.h"
#include "core_manager.h"
#include "thread_manager.h"
#include "thread.h"
#include "instruction.h"
#include "basic_block.h"
#include "performance_model.h"
#include "clock_skew_minimization_object.h"
#include "instruction_decoder.h"
#include "config.hpp"
#include "syscall_model.h"
#include "core.h"
#include "magic_client.h"
#include "branch_predictor.h"

#include <sys/syscall.h>

TraceThread::TraceThread(Thread *thread, String tracefile, String responsefile, app_id_t app_id, bool cleanup)
   : m__thread(NULL)
   , m_thread(thread)
   , m_trace(tracefile.c_str(), responsefile.c_str(), thread->getId())
   , m_trace_has_pa(false)
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
      return m_trace.va2pa(va);
   else
      return (UInt64(m_thread->getId()) << pa_core_shift) | (va & pa_va_mask);
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
      case SYS_exit:
         Sim()->getTraceManager()->endApplication(this);
         break;

      case SYS_futex:
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
   return Sim()->getTraceManager()->createThread(m_app_id);
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
   instruction->setAtomic(xed_operand_values_get_atomic(xed_decoded_inst_operands_const(&xed_inst)));
   char disassembly[40];
   xed_format(m_syntax, &xed_inst, disassembly, sizeof(disassembly), inst.sinst->addr);
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
      int address_idx = 0;

      // Ignore memory-referencing operands in NOP instructions
      if (!xed_decoded_inst_get_attribute(&xed_inst, XED_ATTRIBUTE_NOP))
      {
         for(uint32_t mem_idx = 0; mem_idx < xed_decoded_inst_number_of_memory_operands(&xed_inst); ++mem_idx)
         {
            if (xed_decoded_inst_mem_read(&xed_inst, mem_idx))
            {
               core->accessMemory(
                     /*(is_atomic_update) ? Core::LOCK :*/ Core::NONE,
                     (is_atomic_update) ? Core::READ_EX : Core::READ,
                     va2pa(inst.addresses[address_idx]),
                     NULL,
                     xed_decoded_inst_get_memory_operand_length(&xed_inst, mem_idx),
                     Core::MEM_MODELED_COUNT,
                     va2pa(inst.sinst->addr));
               ++address_idx;
            }
         }

         for(uint32_t mem_idx = 0; mem_idx < xed_decoded_inst_number_of_memory_operands(&xed_inst); ++mem_idx)
         {
            if (xed_decoded_inst_mem_written(&xed_inst, mem_idx))
            {
               if (is_atomic_update)
                  core->logMemoryHit(false, Core::WRITE, va2pa(inst.addresses[address_idx]), Core::MEM_MODELED_COUNT, va2pa(inst.sinst->addr));
               else
                  core->accessMemory(
                        /*(is_atomic_update) ? Core::UNLOCK :*/ Core::NONE,
                        Core::WRITE,
                        va2pa(inst.addresses[address_idx]),
                        NULL,
                        xed_decoded_inst_get_memory_operand_length(&xed_inst, mem_idx),
                        Core::MEM_MODELED_COUNT,
                        va2pa(inst.sinst->addr));
               ++address_idx;
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
}

void TraceThread::run()
{
   Sim()->getThreadManager()->onThreadStart(m_thread->getId(), SubsecondTime::Zero());

   ClockSkewMinimizationClient *client = m_thread->getClockSkewMinimizationClient();
   LOG_ASSERT_ERROR(client != NULL, "Tracing doesn't work without a clock skew minimization scheme"); // as we'd just overrun our basicblock queue

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

   m_trace.Read(inst);
   while(m_trace.Read(next_inst))
   {
      bool do_icache_warmup = false;
      UInt64 icache_warmup_addr = 0, icache_warmup_size = 0;

      // Reconstruct and count basic blocks

      if (m_bbv_end || m_bbv_last != inst.sinst->addr)
      {
         // We're the start of a new basic block
         core->countInstructions(m_bbv_base, m_bbv_count);
         // In cache-only mode, we'll want to do I-cache warmup
         do_icache_warmup = true;
         icache_warmup_addr = m_bbv_base;
         icache_warmup_size = m_bbv_last - m_bbv_base;
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
         case InstMode::BASE:
         case InstMode::FAST_FORWARD:
            break;

         case InstMode::CACHE_ONLY:
            handleInstructionWarmup(inst, next_inst, core, do_icache_warmup, icache_warmup_addr, icache_warmup_size);
            break;

         case InstMode::DETAILED:
            handleInstructionDetailed(inst, next_inst, prfmdl);
            break;
      }


      client->synchronize(SubsecondTime::Zero(), false);

      // We may have been rescheduled to a different core
      core = m_thread->getCore();
      prfmdl = core->getPerformanceModel();

      if (m_stop)
         break;

      inst = next_inst;
   }

   printf("[TRACE:%u] -- %s --\n", m_thread->getId(), m_stop ? "STOP" : "DONE");

   Sim()->getThreadManager()->onThreadExit(m_thread->getId());
   Sim()->getTraceManager()->signalDone(this, m_stop /*aborted*/);
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
