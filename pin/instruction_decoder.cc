#include "instruction_decoder.h"
#include "instruction_latencies.h"

extern "C" {
#include <xed-reg-class.h>
}

#include <signal.h>
#include <assert.h>


  //////////////////////////////////////////
 ///// IMPLEMENTATION OF INSTRUCTIONS /////
//////////////////////////////////////////

InstructionDecoder::InstructionDecoder(): instruction_counter(0), decode_result(new DecodeResult()) {
}

InstructionDecoder::~InstructionDecoder() {
  delete decode_result;
}

InstructionDecoder::DecodeResult& InstructionDecoder::decode(INS ins)
{
   std::set<REG> mem_regs;

   this->decode_result->index = 0;
   this->decode_result->m_ins = ins;
   this->decode_result->instruction_pc = INS_Address(ins);

   this->decode_result->numLoads = 0;
   this->decode_result->numStores = 0;
   for(UINT32 idx = 0; idx < INS_MemoryOperandCount(ins); ++idx)
   {
      std::set<REG> regs;
      regs.insert(INS_OperandMemoryBaseReg(ins, INS_MemoryOperandIndexToOperandIndex(ins, idx)));
      regs.insert(INS_OperandMemoryIndexReg(ins, INS_MemoryOperandIndexToOperandIndex(ins, idx)));

      for(std::set<REG>::iterator it = regs.begin(); it != regs.end(); ++it)
         mem_regs.insert(*it);

      if (INS_MemoryOperandIsRead(ins, idx)) {
         MicroOpRegs uop_data;
         uop_data.regs_src = regs;
         this->decode_result->uop_load.push_back(uop_data);
         this->decode_result->numLoads++;
      }

      if (INS_MemoryOperandIsWritten(ins, idx)) {
         MicroOpRegs uop_data;
         uop_data.regs_src = regs;
         this->decode_result->uop_store.push_back(uop_data);
         this->decode_result->numStores++;
      }
   }

   for(UINT32 idx = 0; idx < INS_OperandCount(ins); ++idx)
   {
      if (! INS_OperandIsReg(ins, idx)) continue;

      REG reg = INS_OperandReg(ins, idx);

      if (INS_OperandRead(ins, idx) && mem_regs.count(reg) == 0)
         this->decode_result->uop_execute.regs_src.insert(reg);
      if (INS_OperandWritten(ins, idx))
         this->decode_result->uop_execute.regs_dst.insert(reg);
   }

   // Not sure if this is needed, the hardware is doing a lot of optimizations on stack operations
   #if 0
   if (INS_IsStackRead(ins) || INS_IsStackWrite(ins))
   {
      // For push/pop, add the stack pointer as explicit destination register
      // (It's already listed as a source memory operand for the load/store, but the modified rsp needs to be written back)
      // (It should be one of the implicit operands, but those are rather difficult to parse correctly)
      this->decode_result->uop_execute.regs_dst.insert(LEVEL_BASE::REG_RSP);
   }
   #endif

   if (INS_Category(ins) == XED_CATEGORY_DATAXFER || INS_Category(ins) == XED_CATEGORY_CMOV
      || INS_Opcode(ins) == XED_ICLASS_PUSH || INS_Opcode(ins) == XED_ICLASS_POP)
   {
      if ((this->decode_result->numLoads + this->decode_result->numStores) == 0)
         this->decode_result->numExecs = 1;
      else
         this->decode_result->numExecs = 0;
   }
   else
   {
      this->decode_result->numExecs = 1;
   }

   this->decode_result->totalMicroOps =
        this->decode_result->numLoads
      + this->decode_result->numExecs
      + this->decode_result->numStores;

   if (toupper(INS_Mnemonic(ins).c_str()[0]) == 'F')
      this->decode_result->is_x87 = true;

   switch(INS_Opcode(ins)) {
      // TODO: There may be more (newer) instructions, but they are all kernel only
      case XED_ICLASS_JMP_FAR:
      case XED_ICLASS_CALL_FAR:
      case XED_ICLASS_RET_FAR:
      case XED_ICLASS_IRET:
      case XED_ICLASS_CPUID:
      case XED_ICLASS_LGDT:
      case XED_ICLASS_LIDT:
      case XED_ICLASS_LLDT:
      case XED_ICLASS_LTR:
      case XED_ICLASS_LMSW:
      case XED_ICLASS_WBINVD:
      case XED_ICLASS_INVD:
      case XED_ICLASS_INVLPG:
      case XED_ICLASS_RSM:
      case XED_ICLASS_WRMSR:
      case XED_ICLASS_SYSENTER:
      case XED_ICLASS_SYSRET:
         this->decode_result->is_serializing = true;
   }

   this->decode_result->instruction_id = instruction_counter;
   instruction_counter++;

   return *(this->decode_result);
}

InstructionDecoder::DecodeResult::DecodeResult(): is_x87(false), is_serializing(false), currentMicroOp(new MicroOp()) {
}

InstructionDecoder::DecodeResult::~DecodeResult() {
  delete currentMicroOp;
}

bool InstructionDecoder::DecodeResult::hasNext() const {
  return index < totalMicroOps;
}

void InstructionDecoder::DecodeResult::addSrcs(std::set<REG> regs) {
   for(std::set<REG>::iterator it = regs.begin(); it != regs.end(); ++it)
      if (REG_valid(*it)) {
         REG reg = REG_FullRegName(*it);
         if (reg == LEVEL_BASE::REG_EIP || reg == LEVEL_BASE::REG_RIP) continue; // eip/rip is known at decode time, shouldn't be a dependency
         currentMicroOp->addSourceRegister((uint32_t)reg, String(REG_StringShort(reg).c_str()));
      }
}

void InstructionDecoder::DecodeResult::addDsts(std::set<REG> regs) {
   for(std::set<REG>::iterator it = regs.begin(); it != regs.end(); ++it)
      if (REG_valid(*it)) {
         REG reg = REG_FullRegName(*it);
         if (reg == LEVEL_BASE::REG_EIP || reg == LEVEL_BASE::REG_RIP) continue; // eip/rip is known at decode time, shouldn't be a dependency
         currentMicroOp->addDestinationRegister((uint32_t)reg, String(REG_StringShort(reg).c_str()));
      }
}

MicroOp& InstructionDecoder::DecodeResult::next()
{
   assert(index >= 0 && index < totalMicroOps);

   currentMicroOp->clear();
   currentMicroOp->setInstructionPointer(Memory::make_access(instruction_pc));

   // We don't necessarily know the address at this point as it could
   // be dependent on register values.  Therefore, fill it in at simulation time.

   if (isLoad(index))
   {
      currentMicroOp->makeLoad(
              index
            , Memory::make_access(0)
            , (xed_iclass_enum_t)INS_Opcode(m_ins)
            , INS_Mnemonic(m_ins).c_str()
            , 0
            );
   }
   else if (isStore(index))
   {
      size_t storeIndex = index - numLoads - numExecs;

      currentMicroOp->makeStore(
              storeIndex
            , numExecs
            , Memory::make_access(0)
            , (xed_iclass_enum_t)INS_Opcode(m_ins)
            , INS_Mnemonic(m_ins).c_str()
            , 0
            );
   }
   else if (isExec(index))
   {
      currentMicroOp->makeExecute(
              0
            , numLoads
            , (xed_iclass_enum_t)INS_Opcode(m_ins)
            , INS_Mnemonic(m_ins).c_str()
            , getInstructionLatency((xed_iclass_enum_t)INS_Opcode(m_ins))
            , INS_IsBranch(m_ins) && INS_HasFallThrough(m_ins) /* is conditional branch? */
            , false /* branch taken: not yet known */);
   }

   // Fill in the destination registers for both loads and executes, also on stores if there are no loads or executes
   if (isLoad(index)) {
      MicroOpRegs uop_data = uop_load[loadNum(index)];
      addSrcs(uop_data.regs_src);
      addDsts(uop_data.regs_dst);

      if (numExecs == 0) {
         // No execute microop: we inherit its read operands
         addSrcs(uop_execute.regs_src);
         if (numStores == 0)
            // No store microop either: we also inherit its write operands
            addDsts(uop_execute.regs_dst);
      }
   }

   if (isExec(index)) {
      addSrcs(uop_execute.regs_src);
      addDsts(uop_execute.regs_dst);
  }

   if (isStore(index)) {
      MicroOpRegs uop_data = uop_store[storeNum(index)];
      addSrcs(uop_data.regs_src);
      addDsts(uop_data.regs_dst);

      if (numExecs == 0) {
         // No execute microop: we inherit its write operands
         addDsts(uop_execute.regs_dst);
         if (numLoads == 0)
            // No load microops either: we also inherit its read operands
            addSrcs(uop_execute.regs_src);
      }
   }

   // Mark if this is the first micro op of an instruction
   if (index == 0)
      currentMicroOp->setFirst(true);

   if (index == 0 && is_x87)
      currentMicroOp->setIsX87(true);

   index++;
   // Interrupts and serializing instructions are added to the last microOp
   if (index == totalMicroOps) {
      // Set the last microOp flag
      currentMicroOp->setLast(true);
      // Since we're user-level, there are no interrupts
      #if 0
      // Check if the instruction is an interrupt, place the interrupt flag
      if (disassembler_instruction->insType != IT_BRANCH
            && disassembler_instruction->insType != IT_CALL
            && disassembler_instruction->insType != IT_RETURN
            && jump) {
         currentMicroOp->setInterrupt(true);
      }
      #endif
      // Check if the instruction is serializing, place the serializing flag
      if (is_serializing) {
         currentMicroOp->setSerializing(true);
      }
   }

#ifndef NDEBUG
   currentMicroOp->setDebugInfo(debugInfo);
#endif

   return *currentMicroOp;
}

bool InstructionDecoder::DecodeResult::isLoad(int index) {
  return (index < numLoads);
}
// Return which load number this is, 0 indexed
int InstructionDecoder::DecodeResult::loadNum(int index)
{
   assert (isLoad(index));
   return (index);
}
bool InstructionDecoder::DecodeResult::isExec(int index) {
  return (index >= numLoads && index < (numLoads + numExecs));
}
// Return which exec number this is, 0 indexed
int InstructionDecoder::DecodeResult::execNum(int index)
{
   assert (isExec(index));
   return (index - numLoads);
}
bool InstructionDecoder::DecodeResult::isStore(int index) {
  return (index >= (numLoads + numExecs));
}
// Return which store number this is, 0 indexed
int InstructionDecoder::DecodeResult::storeNum(int index)
{
   assert(isStore(index));
   return (index - (numLoads + numExecs));
}
