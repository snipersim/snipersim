#include "instruction_modeling.h"
#include "routine_replace.h"
#include "inst_mode_macros.h"
#include "local_storage.h"

#include "simulator.h"
#include "performance_model.h"
#include "core_manager.h"
#include "core.h"
#include "timer.h"
#include "mcp.h"
#include "instruction_decoder.h"
#include "instruction.h"
#include "micro_op.h"
#include "magic_client.h"
#include "inst_mode.h"
#include "dvfs_manager.h"
#include "hooks_manager.h"
#include "branch_predictor.h"

#include <unordered_map>

void InstructionModeling::handleBasicBlock(THREADID thread_id, BasicBlock *sim_basic_block)
{
   PerformanceModel *prfmdl = Sim()->getCoreManager()->getCurrentCore(thread_id)->getPerformanceModel();

   prfmdl->queueBasicBlock(sim_basic_block);

   #ifndef ENABLE_PERF_MODEL_OWN_THREAD
   prfmdl->iterate();
   #endif
}

static void handleBranch(THREADID thread_id, ADDRINT eip, BOOL taken, ADDRINT target)
{
   assert(Sim() && Sim()->getCoreManager() && Sim()->getCoreManager()->getCurrentCore(thread_id));
   PerformanceModel *prfmdl = Sim()->getCoreManager()->getCurrentCore(thread_id)->getPerformanceModel();

   DynamicInstructionInfo info = DynamicInstructionInfo::createBranchInfo(eip, taken, target);
   prfmdl->pushDynamicInstructionInfo(info);
}

static void handleBranchWarming(THREADID thread_id, ADDRINT eip, BOOL taken, ADDRINT target)
{
   PerformanceModel *prfmdl = Sim()->getCoreManager()->getCurrentCore(thread_id)->getPerformanceModel();
   BranchPredictor *bp = prfmdl->getBranchPredictor();

   if (bp) {
      bool prediction = bp->predict(eip, target);
      bp->update(prediction, taken, eip, target);
   }
}

static VOID handleMagic(CONTEXT * ctxt, ADDRINT next_eip)
{
   ADDRINT res = handleMagicInstruction(PIN_GetContextReg(ctxt, REG_GAX),
                    PIN_GetContextReg(ctxt, REG_GBX), PIN_GetContextReg(ctxt, REG_GCX));
   PIN_SetContextReg(ctxt, REG_GAX, res);
   // Forcefully abort the current trace (Redmine #118).
   PIN_SetContextReg(ctxt, REG_INST_PTR, next_eip);
   PIN_ExecuteAt(ctxt);
}

static void handleRdtsc(THREADID thread_id, PIN_REGISTER * gax, PIN_REGISTER * gdx)
{
   Core *core = Sim()->getCoreManager()->getCurrentCore(thread_id);
   assert (core);
   SubsecondTime cycles_fs = core->getPerformanceModel()->getElapsedTime();
   // Convert SubsecondTime to cycles in global clock domain
   const ComponentPeriod *dom_global = Sim()->getDvfsManager()->getGlobalDomain();
   UInt64 cycles = SubsecondTime::divideRounded(cycles_fs, *dom_global);
   // Return in eax and edx
   gdx->dword[0] = cycles >> 32;
   gax->dword[0] = cycles & 0xffffffff;
}

static void fillOperandListMemOps(OperandList *list, INS ins)
{
   // NOTE: This code is written to reflect rewriteStackOp and
   // rewriteMemOp etc from redirect_memory.cc and it MUST BE
   // MAINTAINED to reflect that code.

   if (Sim()->getConfig()->getSimulationMode() == Config::FULL)
   {
      // string ops
      if ((INS_RepPrefix(ins) || INS_RepnePrefix(ins)))
      {
         if (INS_Opcode(ins) == XED_ICLASS_SCASB ||
             INS_Opcode(ins) == XED_ICLASS_CMPSB)
         {
            // handled by StringInstruction
            return;
         }
      }

      // stack ops
      if (INS_Opcode (ins) == XED_ICLASS_PUSH)
      {
         if (INS_OperandIsImmediate (ins, 0))
            list->push_back(Operand(Operand::MEMORY, 0, Operand::WRITE));

         else if (INS_OperandIsReg (ins, 0))
            list->push_back(Operand(Operand::MEMORY, 0, Operand::WRITE));

         else if (INS_OperandIsMemory (ins, 0))
         {
            list->push_back(Operand(Operand::MEMORY, 0, Operand::READ));
            list->push_back(Operand(Operand::MEMORY, 0, Operand::WRITE));
         }
      }

      else if (INS_Opcode (ins) == XED_ICLASS_POP)
      {
         if (INS_OperandIsReg (ins, 0))
         {
            list->push_back(Operand(Operand::MEMORY, 0, Operand::READ));
         }

         else if (INS_OperandIsMemory (ins, 0))
         {
            list->push_back(Operand(Operand::MEMORY, 0, Operand::READ));
            list->push_back(Operand(Operand::MEMORY, 0, Operand::WRITE));
         }
      }

      else if (INS_IsCall (ins))
      {
         if (INS_OperandIsMemory (ins, 0))
         {
            list->push_back(Operand(Operand::MEMORY, 0, Operand::READ));
            list->push_back(Operand(Operand::MEMORY, 0, Operand::WRITE));
         }

         else
            list->push_back(Operand(Operand::MEMORY, 0, Operand::WRITE));
      }

      else if (INS_IsRet (ins))
         list->push_back(Operand(Operand::MEMORY, 0, Operand::READ));

      else if (INS_Opcode (ins) == XED_ICLASS_LEAVE)
         list->push_back(Operand(Operand::MEMORY, 0, Operand::READ));

      else if ((INS_Opcode (ins) == XED_ICLASS_PUSHF) || (INS_Opcode (ins) == XED_ICLASS_PUSHFD))
         list->push_back(Operand(Operand::MEMORY, 0, Operand::WRITE));

      else if ((INS_Opcode (ins) == XED_ICLASS_POPF) || (INS_Opcode (ins) == XED_ICLASS_POPFD))
         list->push_back(Operand(Operand::MEMORY, 0, Operand::READ));

      // mem ops
      else if (INS_IsMemoryRead (ins) || INS_IsMemoryWrite (ins))
      {
         // first all reads (dyninstrinfo pushed from redirectMemOp)
         for (unsigned int i = 0; i < INS_MemoryOperandCount(ins); i++)
         {
            if (INS_MemoryOperandIsRead(ins, i))
               list->push_back(Operand(Operand::MEMORY, 0, Operand::READ));
         }
         // then all writes (dyninstrinfo pushed from completeMemWrite)
         for (unsigned int i = 0; i < INS_MemoryOperandCount(ins); i++)
         {
            if (INS_MemoryOperandIsWritten(ins, i))
               list->push_back(Operand(Operand::MEMORY, 0, Operand::WRITE));
         }
      }
   }

   else // Sim()->getConfig()->getSimulationMode() == Config::LITE
   {
      if (INS_IsMemoryRead (ins) || INS_IsMemoryWrite (ins))
      {
         // first all reads (dyninstrinfo pushed from redirectMemOp)
         for (unsigned int i = 0; i < INS_MemoryOperandCount(ins); i++)
         {
            if (INS_MemoryOperandIsRead(ins, i))
               list->push_back(Operand(Operand::MEMORY, 0, Operand::READ));
         }
         // then all writes (dyninstrinfo pushed from completeMemWrite)
         for (unsigned int i = 0; i < INS_MemoryOperandCount(ins); i++)
         {
            if (INS_MemoryOperandIsWritten(ins, i))
               list->push_back(Operand(Operand::MEMORY, 0, Operand::WRITE));
         }
      }
   }
}

static void fillOperandList(OperandList *list, INS ins)
{
   // memory
   fillOperandListMemOps(list, ins);

   // for handling register operands
   unsigned int max_read_regs = INS_MaxNumRRegs(ins);
   unsigned int max_write_regs = INS_MaxNumWRegs(ins);

   for (unsigned int i = 0; i < max_read_regs; i++)
   {
      REG reg_i = INS_RegR(ins, i);
      if (REG_valid(reg_i))
         list->push_back(Operand(Operand::REG, reg_i, Operand::READ, REG_StringShort(reg_i).c_str(), INS_RegRContain(ins, reg_i)));
   }

   for (unsigned int i = 0; i < max_write_regs; i++)
   {
      REG reg_i = INS_RegW(ins, i);
      if (REG_valid(reg_i))
         list->push_back(Operand(Operand::REG, reg_i, Operand::WRITE, REG_StringShort(reg_i).c_str(), INS_RegWContain(ins, reg_i)));
   }

   // immediate
   for (unsigned int i = 0; i < INS_OperandCount(ins); i++)
   {
      if (INS_OperandIsImmediate(ins, i))
      {
         list->push_back(Operand(Operand::IMMEDIATE, INS_OperandImmediate(ins, i), Operand::READ));
      }
   }
}

std::unordered_map<ADDRINT, std::vector<MicroOp *> > instruction_cache;

BOOL InstructionModeling::addInstructionModeling(TRACE trace, INS ins, BasicBlock *basic_block)
{
   // For all LOCK-prefixed and atomic-update instructions, for timing purposes we add an MFENCE
   // instruction before and after the atomic instruction to force waiting for loads and stores
   // in the timing model
   static Instruction *mfence_instruction = NULL;
   if (! mfence_instruction) {
      OperandList list;
      mfence_instruction = new GenericInstruction(list);
      MicroOp * uop = new MicroOp();
      uop->makeDynamic("MFENCE", 1);
      uop->setMemBarrier(true);
      uop->setFirst(true);
      uop->setLast(true);
      mfence_instruction->addMicroOp(uop);
   }

   // Functional modeling

   // Simics-style magic instruction: xchg bx, bx
   if (INS_IsXchg(ins) && INS_OperandReg(ins, 0) == REG_BX && INS_OperandReg(ins, 1) == REG_BX)
   {
      INS_InsertPredicatedCall(ins, IPOINT_BEFORE, (AFUNPTR)handleMagic, IARG_CONTEXT, IARG_FALLTHROUGH_ADDR, IARG_END);
      // Trace will be aborted after MAGIC (Redmine #118), so don't add the subsequent instructions to the basic-block list.
      return false;
   }

   if (INS_IsRDTSC(ins))
      INS_InsertPredicatedCall(ins, IPOINT_AFTER, (AFUNPTR)handleRdtsc, IARG_THREAD_ID, IARG_REG_REFERENCE, REG_GAX, IARG_REG_REFERENCE, REG_GDX, IARG_END);

   if (INS_IsBranch(ins) && INS_HasFallThrough(ins))
   {
      // In warming mode, warm up the branch predictors
      INSTRUMENT_PREDICATED(
         INSTR_IF_CACHEONLY,
         trace, ins, IPOINT_TAKEN_BRANCH, (AFUNPTR)handleBranchWarming,
         IARG_THREAD_ID,
         IARG_ADDRINT, INS_Address(ins),
         IARG_BOOL, TRUE,
         IARG_BRANCH_TARGET_ADDR,
         IARG_END);

      INSTRUMENT_PREDICATED(
         INSTR_IF_CACHEONLY,
         trace, ins, IPOINT_AFTER, (AFUNPTR)handleBranchWarming,
         IARG_THREAD_ID,
         IARG_ADDRINT, INS_Address(ins),
         IARG_BOOL, FALSE,
         IARG_BRANCH_TARGET_ADDR,
         IARG_END);

      // In detailed mode, push a DynamicInstructionInfo
      INSTRUMENT_PREDICATED(
         INSTR_IF_DETAILED_OR_FULL,
         trace, ins, IPOINT_TAKEN_BRANCH, (AFUNPTR)handleBranch,
         IARG_THREAD_ID,
         IARG_ADDRINT, INS_Address(ins),
         IARG_BOOL, TRUE,
         IARG_BRANCH_TARGET_ADDR,
         IARG_END);

      INSTRUMENT_PREDICATED(
         INSTR_IF_DETAILED_OR_FULL,
         trace, ins, IPOINT_AFTER, (AFUNPTR)handleBranch,
         IARG_THREAD_ID,
         IARG_ADDRINT, INS_Address(ins),
         IARG_BOOL, FALSE,
         IARG_BRANCH_TARGET_ADDR,
         IARG_END);
   }


   if (!basic_block)
      return true;


   // Timing modeling

   if (INS_IsAtomicUpdate(ins))
      basic_block->push_back(mfence_instruction);

   OperandList list;
   fillOperandList(&list, ins);

   // branches
   if (INS_IsBranch(ins) && INS_HasFallThrough(ins))
   {
      basic_block->push_back(new BranchInstruction(list));
   }

   // Now handle instructions which have a static cost
   else
   {
      switch(INS_Opcode(ins))
      {
      case XED_ICLASS_DIV:
         basic_block->push_back(new ArithInstruction(INST_DIV, list));
         break;
      case XED_ICLASS_MUL:
         basic_block->push_back(new ArithInstruction(INST_MUL, list));
         break;
      case XED_ICLASS_FDIV:
         basic_block->push_back(new ArithInstruction(INST_FDIV, list));
         break;
      case XED_ICLASS_FMUL:
         basic_block->push_back(new ArithInstruction(INST_FMUL, list));
         break;

      case XED_ICLASS_SCASB:
      case XED_ICLASS_CMPSB:
         if (Sim()->getConfig()->getSimulationMode() == Config::FULL)
         {
            basic_block->push_back(new StringInstruction(list));
            break;
         }

      default:
         basic_block->push_back(new GenericInstruction(list));
      }
   }

   ADDRINT addr = INS_Address(ins);

   basic_block->back()->setAddress(addr);
   basic_block->back()->setAtomic(INS_IsAtomicUpdate(ins));
   basic_block->back()->setDisassembly(INS_Disassemble(ins).c_str());

   if (instruction_cache.count(addr) == 0)
      instruction_cache[addr] = InstructionDecoder::decode(INS_Address(ins), INS_XedDec(ins), basic_block->back());

   for(std::vector<MicroOp*>::iterator it = instruction_cache[addr].begin(); it != instruction_cache[addr].end(); it++) {
      basic_block->back()->addMicroOp(*it);
   }

   if (INS_IsAtomicUpdate(ins))
      basic_block->push_back(mfence_instruction);

   return true;
}

VOID InstructionModeling::countInstructions(THREADID thread_id, ADDRINT address, INT32 count)
{
   Core* core = localStore[thread_id].core;
   core->countInstructions(address, count);
}

VOID InstructionModeling::accessInstructionCacheWarmup(THREADID threadid, ADDRINT address, UINT32 size)
{
   Core* core = Sim()->getCoreManager()->getCurrentCore(threadid);
   core->readInstructionMemory(address, size);
}
