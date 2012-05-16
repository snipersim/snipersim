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

TraceThread::TraceThread(Thread *thread, String tracefile)
   : m__thread(NULL)
   , m_thread(thread)
   , m_trace(tracefile.c_str())
   , m_stop(false)
   , m_bbv_base(0)
   , m_bbv_count(0)
   , m_output_leftover_size(0)
{
   m_trace.setHandleOutputFunc(TraceThread::__handleOutputFunc, this);

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

BasicBlock* TraceThread::decode(Sift::Instruction &inst)
{
   const xed_decoded_inst_t &xed_inst = inst.sinst->xed_inst;

   OperandList list;

   for(uint32_t mem_idx = 0; mem_idx < xed_decoded_inst_number_of_memory_operands(&xed_inst); ++mem_idx)
      if (xed_decoded_inst_mem_read(&xed_inst, mem_idx))
         list.push_back(Operand(Operand::MEMORY, 0, Operand::READ));

   for(uint32_t mem_idx = 0; mem_idx < xed_decoded_inst_number_of_memory_operands(&xed_inst); ++mem_idx)
      if (xed_decoded_inst_mem_written(&xed_inst, mem_idx))
         list.push_back(Operand(Operand::MEMORY, 0, Operand::WRITE));

   Instruction *instruction;
   if (inst.is_branch)
      instruction = new BranchInstruction(list);
   else
      instruction = new GenericInstruction(list);

   instruction->setAddress(va2pa(inst.sinst->addr));
   instruction->setAtomic(false); // TODO
   char disassembly[40];
   xed_format(m_syntax, &xed_inst, disassembly, sizeof(disassembly), inst.sinst->addr);
   instruction->setDisassembly(disassembly);

   const std::vector<const MicroOp*> *uops = InstructionDecoder::decode(inst.sinst->addr, &xed_inst, instruction);
   instruction->setMicroOps(uops);

   BasicBlock *basic_block = new BasicBlock();
   basic_block->push_back(instruction);

   return basic_block;
}

void TraceThread::run()
{
   m_barrier->wait();

   Sim()->getThreadManager()->onThreadStart(m_thread->getId(), SubsecondTime::Zero());

   ClockSkewMinimizationClient *client = m_thread->getClockSkewMinimizationClient();
   LOG_ASSERT_ERROR(client != NULL, "Tracing doesn't work without a clock skew minimization scheme"); // as we'd just overrun our basicblock queue

   if (m_thread->getCore() == NULL)
   {
      // We didn't get scheduled on startup, wait here
      SubsecondTime time = SubsecondTime::Zero();
      m_thread->reschedule(time, NULL);
   }

   Core *core = m_thread->getCore();
   PerformanceModel *prfmdl = core->getPerformanceModel();

   Sift::Instruction inst;
   while(m_trace.Read(inst))
   {
      const xed_decoded_inst_t &xed_inst = inst.sinst->xed_inst;

      // Push basic block containing this instruction

      if (m_icache.count(inst.sinst->addr) == 0)
         m_icache[inst.sinst->addr] = decode(inst);
      BasicBlock *basic_block = m_icache[inst.sinst->addr];

      prfmdl->queueBasicBlock(basic_block);


      // Reconstruct and count basic blocks

      if (m_bbv_base == 0)
      {
         // We're the start of a new basic block
         m_bbv_base = inst.sinst->addr;
      }
      m_bbv_count++;
      if (inst.is_branch)
      {
         // We're the end of a basic block
         core->countInstructions(m_bbv_base, m_bbv_count);
         m_bbv_base = 0; // Next instruction will start a new basic block
         m_bbv_count = 0;
      }


      // Push dynamic instruction info

      if (inst.is_branch)
      {
         DynamicInstructionInfo info = DynamicInstructionInfo::createBranchInfo(va2pa(inst.sinst->addr), inst.taken, 0 /* TODO: target */);
         prfmdl->pushDynamicInstructionInfo(info);
      }

      for(uint32_t mem_idx = 0; mem_idx < xed_decoded_inst_number_of_memory_operands(&xed_inst); ++mem_idx)
      {
         if (xed_decoded_inst_mem_read(&xed_inst, mem_idx))
         {
            DynamicInstructionInfo info = DynamicInstructionInfo::createMemoryInfo(va2pa(inst.sinst->addr), inst.executed, SubsecondTime::Zero(), va2pa(inst.addresses[mem_idx]), xed_decoded_inst_get_memory_operand_length(&xed_inst, mem_idx), Operand::READ, 0, HitWhere::UNKNOWN);
            prfmdl->pushDynamicInstructionInfo(info);
         }
      }

      for(uint32_t mem_idx = 0; mem_idx < xed_decoded_inst_number_of_memory_operands(&xed_inst); ++mem_idx)
      {
         if (xed_decoded_inst_mem_written(&xed_inst, mem_idx))
         {
            DynamicInstructionInfo info = DynamicInstructionInfo::createMemoryInfo(va2pa(inst.sinst->addr), inst.executed, SubsecondTime::Zero(), va2pa(inst.addresses[mem_idx]), xed_decoded_inst_get_memory_operand_length(&xed_inst, mem_idx), Operand::WRITE, 0, HitWhere::UNKNOWN);
            prfmdl->pushDynamicInstructionInfo(info);
         }
      }


      // simulate

      prfmdl->iterate();

      client->synchronize(SubsecondTime::Zero(), false);
      // We may have been rescheduled to a different core
      core = m_thread->getCore();
      prfmdl = core->getPerformanceModel();

      if (m_stop)
         break;
   }

   printf("[TRACE:%u] -- %s --\n", m_thread->getId(), m_stop ? "STOP" : "DONE");

   Sim()->getThreadManager()->onThreadExit(m_thread->getId());
   Sim()->getTraceManager()->signalDone(m_thread->getId());

   m_barrier->wait();
}

void TraceThread::spawn(Barrier *barrier)
{
   m_barrier = barrier;
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
