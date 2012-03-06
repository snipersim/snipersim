#include "trace_thread.h"
#include "trace_manager.h"
#include "simulator.h"
#include "core_manager.h"
#include "thread_manager.h"
#include "instruction.h"
#include "basic_block.h"
#include "performance_model.h"
#include "clock_skew_minimization_object.h"
#include "instruction_decoder.h"
#include "config.hpp"

TraceThread::TraceThread(core_id_t core_id, String tracefile)
   : m_thread(NULL)
   , m_core(Sim()->getCoreManager()->getCoreFromID(core_id))
   , m_trace(tracefile.c_str())
   , m_stop(false)
   , m_bbv_base(0)
   , m_bbv_count(0)
   , m_output_leftover_size(0)
{
   m_trace.setHandleOutputFunc(TraceThread::__handleOutputFunc, this);

   String syntax = Sim()->getCfg()->getString("general/syntax", "intel");
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
   delete m_thread;
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

      fprintf(fp, "[TRACE:%u] ", m_core->getId());
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
   char *disassembly = new char[40];
   xed_format(m_syntax, &xed_inst, disassembly, 40, inst.sinst->addr);
   instruction->setDisassembly(disassembly);

   std::vector<MicroOp*> uops = InstructionDecoder::decode(inst.sinst->addr, &xed_inst, instruction);

   for(std::vector<MicroOp*>::iterator it = uops.begin(); it != uops.end(); it++)
      instruction->addMicroOp(*it);

   BasicBlock *basic_block = new BasicBlock();
   basic_block->push_back(instruction);

   return basic_block;
}

void TraceThread::run()
{
   m_barrier->wait();

   PerformanceModel *prfmdl = m_core->getPerformanceModel();
   ClockSkewMinimizationClient *client = m_core->getClockSkewMinimizationClient();
   LOG_ASSERT_ERROR(client != NULL, "Tracing doesn't work without a clock skew minimization scheme"); // as we'd just overrun our basicblock queue

   ThreadSpawnRequest req = { MCP_MESSAGE_THREAD_SPAWN_REQUEST_FROM_REQUESTER,
                              NULL /* func */, NULL /* arg */, INVALID_CORE_ID /* requester */,
                              m_core->getId() /* target core_id */, SubsecondTime::Zero() /* start time */};
   Sim()->getThreadManager()->onThreadStart(&req);

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
         m_core->countInstructions(m_bbv_base, m_bbv_count);
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

      if (m_stop)
         break;
   }

   printf("[TRACE:%u] -- %s --\n", m_core->getId(), m_stop ? "STOP" : "DONE");

   Sim()->getThreadManager()->onThreadExit();
   Sim()->getTraceManager()->signalDone(m_core->getId());

   m_barrier->wait();
}

void TraceThread::spawn(Barrier *barrier)
{
   m_barrier = barrier;
   m_thread = Thread::create(this);
   m_thread->run();
}
