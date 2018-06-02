#include "instruction_decoder_wlib.h"
#include "instruction.h"
#include "micro_op.h"
#include "simulator.h"
#include <x86_decoder.h>  // TODO delete

//extern "C" {
//#include <xed-reg-class.h>
//#if PIN_REV >= 62732
//# include <xed-decoded-inst-api.h>
//#endif
//}

void InstructionDecoder::addSrcs(std::set<dl::Decoder::decoder_reg> regs, MicroOp * currentMicroOp) {
   dl::Decoder *dec = Sim()->getDecoder();

   for(std::set<dl::Decoder::decoder_reg>::iterator it = regs.begin(); it != regs.end(); ++it)
      if (!(Sim()->getDecoder()->invalid_register(*it))) {
         dl::Decoder::decoder_reg reg = dec->largest_enclosing_register(*it);
         if (dec->reg_is_program_counter(reg)) continue; // eip/rip is known at decode time, shouldn't be a dependency
         currentMicroOp->addSourceRegister(reg, String(dec->reg_name(reg)));
      }
}

void InstructionDecoder::addAddrs(std::set<dl::Decoder::decoder_reg> regs, MicroOp * currentMicroOp) {
   dl::Decoder *dec = Sim()->getDecoder();

   for(std::set<dl::Decoder::decoder_reg>::iterator it = regs.begin(); it != regs.end(); ++it)
      if (!(dec->invalid_register(*it))) {
         dl::Decoder::decoder_reg reg = dec->largest_enclosing_register(*it);
         if (dec->reg_is_program_counter(reg)) continue; // eip/rip is known at decode time, shouldn't be a dependency
         currentMicroOp->addAddressRegister(reg, String(dec->reg_name(reg)));
      }
}

void InstructionDecoder::addDsts(std::set<dl::Decoder::decoder_reg> regs, MicroOp * currentMicroOp) {
   dl::Decoder *dec = Sim()->getDecoder();

   for(std::set<dl::Decoder::decoder_reg>::iterator it = regs.begin(); it != regs.end(); ++it)
      if (!(dec->invalid_register(*it))) {
         dl::Decoder::decoder_reg reg = dec->largest_enclosing_register(*it);
         if (dec->reg_is_program_counter(reg)) continue; // eip/rip is known at decode time, shouldn't be a dependency
         currentMicroOp->addDestinationRegister(reg, String(dec->reg_name(reg)));
      }
}

unsigned int InstructionDecoder::getNumExecs(const dl::DecodedInst *ins, int numLoads, int numStores)
{
   return Sim()->getDecoder()->get_exec_microops(ins, numLoads, numStores);
}


  //////////////////////////////////////////
 ///// IMPLEMENTATION OF INSTRUCTIONS /////
//////////////////////////////////////////

const std::vector<const MicroOp*>* InstructionDecoder::decode(IntPtr address,  const dl::DecodedInst *ins, Instruction *ins_ptr)
{
   dl::Decoder *dec = Sim()->getDecoder();
   // Determine register dependencies and number of microops per type

   std::vector<std::set<dl::Decoder::decoder_reg> > regs_loads, regs_stores;
   std::set<dl::Decoder::decoder_reg> regs_mem, regs_src, regs_dst;
   std::vector<uint16_t> memop_load_size, memop_store_size;

   int numLoads = 0;
   int numExecs = 0;
   int numStores = 0;

   // Ignore memory-referencing operands in NOP instructions
   if (!(ins->is_nop()))
   {
      for(uint32_t mem_idx = 0; mem_idx < dec->num_memory_operands(ins); ++mem_idx)
      {
         std::set<dl::Decoder::decoder_reg> regs;
         regs.insert(dec->mem_base_reg(ins, mem_idx));
         regs.insert(dec->mem_index_reg(ins, mem_idx));

         if (dec->op_read_mem(ins, mem_idx)) {
            regs_loads.push_back(regs);
            memop_load_size.push_back(dec->size_mem_op(ins, mem_idx));
            numLoads++;
         }

         if (dec->op_write_mem(ins, mem_idx)) {
            regs_stores.push_back(regs);
            memop_store_size.push_back(dec->size_mem_op(ins, mem_idx));
            numStores++;
         }

         regs_mem.insert(regs.begin(), regs.end());
      }
   }

   bool is_atomic = false;
   if (ins->is_atomic())
      is_atomic = true;

   for(uint32_t idx = 0; idx < dec->num_operands(ins); ++idx)
   {
      if (dec->is_addr_gen(ins, idx))
      {
         /* LEA-like instruction */
         regs_src.insert(regs_mem.begin(), regs_mem.end());
      }
      else if (dec->op_is_reg(ins, idx))  
      {
         dl::Decoder::decoder_reg reg = dec->get_op_reg(ins, idx);

         if (dec->op_read_reg(ins, idx) && regs_mem.count(reg) == 0)
            regs_src.insert(reg);
         if (dec->op_write_reg(ins, idx))
            regs_dst.insert(reg);
      }
   }

   // Not sure if this is needed, the hardware is doing a lot of optimizations on stack operations
   #if 0
   //if (INS_IsStackRead(ins) || INS_IsStackWrite(ins))
   //{
      // For push/pop, add the stack pointer as explicit destination register
      // (It's already listed as a source memory operand for the load/store, but the modified rsp needs to be written back)
      // (It should be one of the implicit operands, but those are rather difficult to parse correctly)
   //   regs_dst.insert(XED_REG_RSP);
   //}
   #endif

   numExecs = getNumExecs(ins, numLoads, numStores);

   // Determine some extra instruction characteristics that will affect timing

   // Determine instruction operand width
   uint16_t operand_size = dec->get_operand_size(ins);


   bool is_serializing = ins->is_serializing();

   // Generate list of microops

   std::vector<const MicroOp*> *uops = new std::vector<const MicroOp*>(); //< Return value
   int totalMicroOps = numLoads + numExecs + numStores;
   // FIXME: 
   // Capstone bug: random incorrect disassembly --> ldr x1, [x0] to ldr w1, #0x7faa399350
   // Only happening once. Treat that load as an exec instruction.
   if (totalMicroOps == 0) {
     numExecs = totalMicroOps = 1;
   }
   
   for(int index = 0; index < totalMicroOps; ++index)
   {
      MicroOp *currentMicroOp = new MicroOp();
      // pass the decoder object to allow access to the library 
      currentMicroOp->setInstructionPointer(Memory::make_access(address));
      
      // Extra information on all micro ops 
      currentMicroOp->setOperandSize(operand_size);
      currentMicroOp->setInstruction(ins_ptr);
      currentMicroOp->setDecodedInstruction(ins);
      // We don't necessarily know the address at this point as it could
      // be dependent on register values.  Therefore, fill it in at simulation time.

      if (index < numLoads) /* LOAD */
      {      
         size_t loadIndex = index;
         currentMicroOp->makeLoad(
                 loadIndex
               , ins->inst_num_id()
               , dec->inst_name(ins->inst_num_id())
               , memop_load_size[loadIndex]
               );
      }
      else if (index < numLoads + numExecs) /* EXEC */
      {      
         size_t execIndex = index - numLoads;
         LOG_ASSERT_ERROR(numExecs <= 2, "More than 2 exec uops"); 
         currentMicroOp->makeExecute(
                 execIndex
               , numLoads
               , ins->inst_num_id()
               , dec->inst_name(ins->inst_num_id())
               , ins->is_conditional_branch() /* is conditional branch? */);
      }
      else /* STORE */
      {      
         size_t storeIndex = index - numLoads - numExecs;
         currentMicroOp->makeStore(
                 storeIndex
               , numExecs
               , ins->inst_num_id()
               , dec->inst_name(ins->inst_num_id())
               , memop_store_size[storeIndex]
               );
         if (is_atomic)
            currentMicroOp->setMemBarrier(true);
      }


      // Fill in the destination registers for both loads and executes, also on stores if there are no loads or executes
      if (index < numLoads) /* LOAD */
      {      
         size_t loadIndex = index;
         addSrcs(regs_loads[loadIndex], currentMicroOp);
         addAddrs(regs_loads[loadIndex], currentMicroOp);

         if (numExecs == 0) {
            // No execute microop: we inherit its read operands
            addSrcs(regs_src, currentMicroOp);
            if (numStores == 0)
               // No store microop either: we also inherit its write operands
               addDsts(regs_dst, currentMicroOp);
         }

      }

      else if (index < numLoads + numExecs) /* EXEC */
      {      
         addSrcs(regs_src, currentMicroOp);
         addDsts(regs_dst, currentMicroOp);

         if (ins->is_barrier())
            currentMicroOp->setMemBarrier(true);

         // Special cases
         if (ins->src_dst_merge())
         {
            // In this case, we have a memory to XMM load, where the result merges the source and destination
            addSrcs(regs_dst, currentMicroOp);
         }
      }

      else /* STORE */
      {      
         size_t storeIndex = index - numLoads - numExecs;
         addSrcs(regs_stores[storeIndex], currentMicroOp);
         addAddrs(regs_stores[storeIndex], currentMicroOp);

         if (numExecs == 0) {
            // No execute microop: we inherit its write operands
            addDsts(regs_dst, currentMicroOp);
            if (numLoads == 0)
               // No load microops either: we also inherit its read operands
               addSrcs(regs_src, currentMicroOp);
         }
         if (is_atomic)
            currentMicroOp->setMemBarrier(true);
      }


      /* Extra information of first micro op */

      if (index == 0)
      {
         currentMicroOp->setFirst(true);

         // Use of x87 FPU?
         if (ins->is_X87())
            currentMicroOp->setIsX87(true);
      }


      /* Extra information on last micro op */

      if (index == totalMicroOps - 1) {
         currentMicroOp->setLast(true);

         // Interrupts and serializing instructions are added to the last microOp
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
         if (is_serializing)
            currentMicroOp->setSerializing(true);
      }

      uops->push_back(currentMicroOp);
   }

   return uops;
}
