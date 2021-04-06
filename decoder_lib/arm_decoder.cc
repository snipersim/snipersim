#include "arm_decoder.h"
#include <iostream>
#include <cstring>

namespace dl 
{
  
ARMDecoder::ARMDecoder(dl_arch arch, dl_mode mode, dl_syntax syntax)
{
  cs_err err;
  cs_arch a;
  cs_mode m, m_aux;
  
  // Choose Capstone's architecture and mode to open the library handle
  if (arch == DL_ARCH_ARMv7) {
    a = CS_ARCH_ARM;
    m = (cs_mode)CS_MODE_ARM; //(CS_MODE_ARM + CS_MODE_MCLASS);
    // Second handle to be able to decode ARM and Thumb instructions with the same Decoder instance
    m_aux = (cs_mode)(CS_MODE_ARM + CS_MODE_THUMB);
    err = cs_open(a, m_aux, &m_handle_aux);
    if (err) {
      printf("[DecoderLib] Failed on cs_open() with error: %u\n", err);
      exit(1);
    }
    cs_option(m_handle_aux, CS_OPT_DETAIL, CS_OPT_ON);
    this->m_isa = DL_ISA_ARM32;
  }
  else if (arch == DL_ARCH_ARMv8) {
    a = CS_ARCH_ARM64;
    if (mode == DL_MODE_32) {
      m = CS_MODE_V8;  // 32-bit emulation on 64-bit ARM
      this->m_isa = DL_ISA_V8_32;
    } else {
      m = CS_MODE_ARM;
      this->m_isa = DL_ISA_AARCH64;
    }
  } else {
    printf("[DecoderLib] ARM Decoder architecture (dl_arch) is not valid: %d\n", arch);
    exit(1);
  }
  // Opening main handle with current configuration  
  err = cs_open(a, m, &m_handle);
  if (err) {
    printf("[DecoderLib] Failed on cs_open() with error: %u\n", err);
    exit(1);
  }

  cs_option(m_handle, CS_OPT_DETAIL, CS_OPT_ON);
  
  // TODO right now we only need the default syntax of capstone, so do not set
  // cs_option(handle, CS_OPT_SYNTAX, syntax); 
  
  this->m_arch = arch;
  this->m_mode = mode;
  this->m_syntax = syntax;
}

ARMDecoder::~ARMDecoder()
{
  cs_close(&m_handle);
  cs_close(&m_handle_aux);
}

void ARMDecoder::decode(DecodedInst * inst)
{
  bool res;
  if(inst->get_already_decoded())
    return;
  
  res = cs_disasm_iter(m_handle, const_cast<const uint8_t**>(&(inst->get_code())), &(inst->get_size()), &(inst->get_address()),
                        ((ARMDecodedInst *)inst)->get_capstone_inst());
  assert(res);
  
  inst->set_already_decoded(true);

  ((ARMDecodedInst *)inst)->set_disassembly();

}

// Use this function to avoid isa conflicts with multithreading
void ARMDecoder::decode(DecodedInst * inst, dl_isa isa)
{
  bool res = -1;

  if(inst->get_already_decoded())
    return;
  
/*  const uint8_t* s = inst->get_code();
  for(int i = 0; i < (int)inst->get_size(); i++)
    printf("%02x", (unsigned int) s[i]);
  printf(" %lx\n", inst->get_address());*/
  
  if (isa == DL_ISA_THUMB)
  {
    res = cs_disasm_iter(m_handle_aux, const_cast<const uint8_t**>(&(inst->get_code())), &(inst->get_size()), &(inst->get_address()),
                        ((ARMDecodedInst *)inst)->get_capstone_inst());
  }
  else  // All the other ISA modes use the main handle
  {
    res = cs_disasm_iter(m_handle, const_cast<const uint8_t**>(&(inst->get_code())), &(inst->get_size()), &(inst->get_address()),
                        ((ARMDecodedInst *)inst)->get_capstone_inst());
  }
  assert(res);

  ((ARMDecodedInst *)inst)->set_disassembly();
  inst->set_already_decoded(true);
}

void ARMDecoder::change_isa_mode(dl_isa new_isa)
{
  if (new_isa != this->m_isa)
  {
    if (new_isa == DL_ISA_ARM32)
      cs_option(m_handle, CS_OPT_MODE, CS_MODE_ARM);//(CS_MODE_ARM + CS_MODE_MCLASS));
    else if (new_isa == DL_ISA_THUMB)
      cs_option(m_handle, CS_OPT_MODE, (CS_MODE_ARM + CS_MODE_THUMB));
    this->m_isa = new_isa;
  }
}

const char* ARMDecoder::inst_name(unsigned int inst_id)
{
  const char* ret = cs_insn_name(m_handle, inst_id);
  if (ret == NULL)  // Fix for bug in library
  {
    if (inst_id == 65535)
      ret = "negs";
    else if (inst_id == 65534)
      ret = "ngcs";
  }
  return ret;
}

const char* ARMDecoder::reg_name(unsigned int reg_id)
{
  return cs_reg_name(m_handle, reg_id);
}

Decoder::decoder_reg ARMDecoder::largest_enclosing_register(Decoder::decoder_reg r)
{
  return r;
}

bool ARMDecoder::invalid_register(decoder_reg r)
{
  if (m_arch == DL_ARCH_ARMv7)
    return (r == ARM_REG_INVALID); 
  else 
    return (r == ARM64_REG_INVALID);
}
    
bool ARMDecoder::reg_is_program_counter(decoder_reg r)
{
  // Note: in AArch64 the program counter is not a general purpose register
  if (m_arch == DL_ARCH_ARMv7)
    return r == ARM_REG_PC;
  else 
    return false;
}

const csh & ARMDecoder::get_handle() const
{
  return m_handle;
}

bool ARMDecoder::inst_in_group(const DecodedInst * inst, unsigned int group_id)
{
  return cs_insn_group(m_handle, ((ARMDecodedInst *)inst)->get_capstone_inst(), group_id);
}

unsigned int ARMDecoder::num_operands(const DecodedInst * inst)
{
  cs_insn * csi = ((ARMDecodedInst *)inst)->get_capstone_inst();
  int count;
  if (m_arch == DL_ARCH_ARMv7)
  {
    cs_arm * arm = &(csi->detail->arm);
    count = arm->op_count;
  } else {
    cs_arm64 * arm64 = &(csi->detail->arm64);
    count = arm64->op_count;
  }
  if (count < 0) {
    printf("[DecoderLib] Error: num_operands < 0 \n");
    return 0;
  }
  return (unsigned)count;  
}

unsigned int ARMDecoder::num_memory_operands(const DecodedInst * inst)
{
  int count;
  if (m_arch == DL_ARCH_ARMv7)
  {
    count = cs_op_count(m_handle, ((ARMDecodedInst *)inst)->get_capstone_inst(), ARM_OP_MEM);
  } 
  else  // DL_ARCH_ARMv8
  {
    count = cs_op_count(m_handle, ((ARMDecodedInst *)inst)->get_capstone_inst(), ARM64_OP_MEM);
    // Load and store pair perform two memory accesses with only one memory operand.
    // We consider the second access as an additional memory operand.
    //FIXME: There is a dependency problem when a mem pair acces two different cache lines.
    //This is because the two micro ops are marked as writing both registers (but every access writes to a different one),
    //so they have an intra instruction dependency. This generates a run time error because the variable that stores the number
    //of intra instruction
    //dependencies doesn't take into account this specific case. So, since this is a very particular case and would require
    //more time than I currently have to fix it, I will disable the second memory access.
    //if ( ((ARMDecodedInst *)inst)->is_mem_pair() )
    //  count++;
  }
  if (count < 0) {
    printf("[DecoderLib] Error: num_memory_operands < 0 \n");
    return 0;
  }
  return (unsigned)count;  
}

int ARMDecoder::index_mem_op(cs_insn * csi, unsigned int mem_idx)
{
  if (m_arch == DL_ARCH_ARMv7)
  {
    return cs_op_index(m_handle, csi, ARM_OP_MEM, mem_idx+1);
  }
  else
  {
    unsigned int ins_id = csi->id;
    if ( (ins_id >= ARM64_INS_LDNP && ins_id <= ARM64_INS_LDPSW) ||
            (ins_id == ARM64_INS_STNP) || (ins_id == ARM64_INS_STP) )
    {
      // If it's the second element of a pair memory access, give the index of the first element, 
      // since it's the only memory operand that really exists for Capstone.
      int mi = mem_idx;
      if( mi >= cs_op_count(m_handle, csi, ARM64_OP_MEM))
        mi--;
      return cs_op_index(m_handle, csi, ARM64_OP_MEM, mi+1);
    }
    else
    {
      return cs_op_index(m_handle, csi, ARM64_OP_MEM, mem_idx+1);
    }
  }
}
  
Decoder::decoder_reg ARMDecoder::mem_base_reg (const DecodedInst * inst, unsigned int mem_idx)
{
  cs_insn * csi = ((ARMDecodedInst *)inst)->get_capstone_inst();
  int index = this->index_mem_op(csi, mem_idx);
  if (m_arch == DL_ARCH_ARMv7)
  {
    cs_arm * arm = &(csi->detail->arm);
    return arm->operands[index].mem.base;  
  } else {
    cs_arm64 * arm64 = &(csi->detail->arm64);
    return arm64->operands[index].mem.base;  
  }
}

bool ARMDecoder::mem_base_upate(const DecodedInst *inst, unsigned int mem_idx) {
    cs_insn * csi = ((ARMDecodedInst *)inst)->get_capstone_inst();

    //TODO: Adapt for AMRV7
    if (m_arch == DL_ARCH_ARMv8) {
        switch (csi->id) {
        case ARM64_INS_LDRB:  // Load and store instructions that may use exec units for writebacks
        case ARM64_INS_LDR:
        case ARM64_INS_LDRH:
        case ARM64_INS_LDRSB:
        case ARM64_INS_LDRSH:
        case ARM64_INS_LDRSW:
        case ARM64_INS_STRB:
        case ARM64_INS_STR:
        case ARM64_INS_STRH:
        case ARM64_INS_LDP:
        case ARM64_INS_LDPSW:
        case ARM64_INS_STP:
            return csi->detail->arm64.writeback;
        default:
            return false;
        }
    }
    return false;
}

bool ARMDecoder::has_index_reg(const DecodedInst *inst, unsigned int mem_idx) {
   cs_insn * csi = ((ARMDecodedInst *)inst)->get_capstone_inst();
   cs_detail* detail = csi->detail;
   int index = this->index_mem_op(csi, mem_idx);
   if (m_arch == DL_ARCH_ARMv8) {
      return (detail->arm64.operands[index].mem.index > ARM64_REG_INVALID &&
            detail->arm64.operands[index].mem.index < ARM64_REG_ENDING);
   }
   else //TODO: Fix this
      return true;
}

Decoder::decoder_reg ARMDecoder::mem_index_reg (const DecodedInst * inst, unsigned int mem_idx)
{
  cs_insn * csi = ((ARMDecodedInst *)inst)->get_capstone_inst();
  int index = this->index_mem_op(csi, mem_idx);
  if (m_arch == DL_ARCH_ARMv7)
  {
    cs_arm * arm = &(csi->detail->arm);
    return arm->operands[index].mem.index;  
  } 
  else 
  {
    cs_arm64 * arm64 = &(csi->detail->arm64);
    Decoder::decoder_reg ret_reg;
    ret_reg = arm64->operands[index].mem.index;  
    if (((ARMDecodedInst *)inst)->is_mem_pair())
    {
      // Check if this is the second memory access of an LDP/STP; idx bigger than real memory operands.
      // If so, increment index by size.
      if( (int)mem_idx >= cs_op_count(m_handle, ((ARMDecodedInst *)inst)->get_capstone_inst(), ARM64_OP_MEM))
      {
        ret_reg += size_mem_reg(inst);
      }
    }
    return ret_reg;
  }
}

bool ARMDecoder::op_read_mem(const DecodedInst * inst, unsigned int mem_idx) 
{
  bool ret = false;
  // Double check first that this is a memory operand
  if (this->index_mem_op(((ARMDecodedInst *)inst)->get_capstone_inst(), mem_idx) != -1)
  {
    // If this operation is a load, we must be reading from memory
    int inst_id = ((ARMDecodedInst *)inst)->get_capstone_inst()->id;
    if (m_arch == DL_ARCH_ARMv7)
    {
      if ((inst_id >= ARM_INS_LDA && inst_id <= ARM_INS_LDR) ||
          (inst_id >= ARM_INS_VLD1 && inst_id <= ARM_INS_VLDR))
            ret = true;
    } else {  // AARCH64
      if (inst_id >= ARM64_INS_LD1 && inst_id <= ARM64_INS_LDXR)
        ret = true;
    }  
  } 
  return ret;
}

bool ARMDecoder::op_write_mem(const DecodedInst * inst, unsigned int mem_idx)
{
  bool ret = false;
  // Double check first that this is a memory operand
  if (this->index_mem_op(((ARMDecodedInst *)inst)->get_capstone_inst(), mem_idx) != -1)
  {
    // If this operation is a store, we must be writing to memory
    int inst_id = ((ARMDecodedInst *)inst)->get_capstone_inst()->id;
    if (m_arch == DL_ARCH_ARMv7)
    {
      if ((inst_id >= ARM_INS_STC2L && inst_id <= ARM_INS_STR) ||
          (inst_id >= ARM_INS_VST1 && inst_id <= ARM_INS_VSTR))
            ret = true;  
    } else {
      if (inst_id >= ARM64_INS_ST1 && inst_id <= ARM64_INS_STXR)
        ret = true;
    }
  }
  return ret;
}

bool ARMDecoder::op_read_reg (const DecodedInst * inst, unsigned int idx)
{

  cs_insn * csi = ((ARMDecodedInst *)inst)->get_capstone_inst();
  cs_detail *detail = csi->detail;

  switch (m_arch == DL_ARCH_ARMv8 ? detail->arm64.operands[idx].access : detail->arm.operands[idx].access) {
  case CS_AC_READ:
      return true;
  case CS_AC_READ | CS_AC_WRITE:
      return true;
  default:
      switch (csi->id) {
      //Capstone bug, the first register of the cmp instruction is marked as
      //a write register, but it is a read register instead.
      case ARM64_INS_CMP:
          return true;
      }
      return false;
  }

  decoder_reg reg = get_op_reg(inst, idx);
  if (detail->regs_read_count > 0) 
  {
	for (int n = 0; n < detail->regs_read_count; n++) 
    {
      if(detail->regs_read[n] == reg)
        return true;
	}
  }
  return false;
}

bool ARMDecoder::op_write_reg (const DecodedInst * inst, unsigned int idx)
{
   cs_insn * csi = ((ARMDecodedInst *)inst)->get_capstone_inst();
   cs_detail *detail = csi->detail;

   switch (m_arch == DL_ARCH_ARMv8 ? detail->arm64.operands[idx].access : detail->arm.operands[idx].access) {
   case CS_AC_WRITE:
      switch (csi->id) {
      //Capstone bug, the first register of the cmp instruction is marked as
      //a write register, but it is a read register instead.
      case ARM64_INS_CMP:
         return false;
      }
      return true;
   case CS_AC_READ | CS_AC_WRITE:
      switch (csi->id) {
      //Capstone bug, the first register of the str instruction is marked as
      //a read/write register, but it is a read register instead.
      case ARM64_INS_STR:
      case ARM64_INS_STP:
         return false;
      }
      return true;
   default:
      return false;
   }

   decoder_reg reg = get_op_reg(inst, idx);
   if (detail->regs_write_count > 0)
   {
      for (int n = 0; n < detail->regs_write_count; n++)
      {
         if(detail->regs_write[n] == reg)
            return true;
      }
   }
   return false;

}

bool ARMDecoder::is_addr_gen(const DecodedInst * inst, unsigned int idx)
{
    //Address generation instructions don't have any memory operand, so this should
    //always return false in order to generate the right register dependencies
  /*int inst_id = ((ARMDecodedInst *)inst)->get_capstone_inst()->id;
  // No LEA-like instructions in ARM32
  if (m_arch == DL_ARCH_ARMv8 && (inst_id == ARM64_INS_ADR || inst_id == ARM64_INS_ADRP))
  {
    return true;
  }*/
  return false;
}

bool ARMDecoder::op_is_reg (const DecodedInst * inst, unsigned int idx)
{
  if (m_arch == DL_ARCH_ARMv7)
  {
    cs_arm * arm = &(((ARMDecodedInst *)inst)->get_capstone_inst()->detail->arm);
    cs_arm_op *op = &(arm->operands[idx]);
    return (op->type == ARM_OP_REG);
  } else {
    cs_arm64 * arm64 = &(((ARMDecodedInst *)inst)->get_capstone_inst()->detail->arm64);
    cs_arm64_op *op = &(arm64->operands[idx]);
    return (op->type == ARM64_OP_REG);
  }
}  

Decoder::decoder_reg ARMDecoder::get_op_reg (const DecodedInst * inst, unsigned int idx)
{
  if (m_arch == DL_ARCH_ARMv7)
  {
    cs_arm * arm = &(((ARMDecodedInst *)inst)->get_capstone_inst()->detail->arm);
    cs_arm_op *op = &(arm->operands[idx]);
    return op->reg;
  } else {
    cs_arm64 * arm64 = &(((ARMDecodedInst *)inst)->get_capstone_inst()->detail->arm64);
    cs_arm64_op *op = &(arm64->operands[idx]);
    return op->reg;
  }
}

unsigned int ARMDecoder::size_mem_reg(const DecodedInst * inst)
{
  unsigned int size_reg = 1;
  cs_insn* ci = ((ARMDecodedInst *)inst)->get_capstone_inst();
  if (m_arch == DL_ARCH_ARMv7)
    size_reg = 4;  // TODO FIXME
  else
  {
    cs_arm64 *arm64 = &(ci->detail->arm64);
    for (int i = 0; i < arm64->op_count; i++) 
    {
      cs_arm64_op *op = &(arm64->operands[i]);
      if (op->type == ARM64_OP_REG)
      {
        if(op->reg >= ARM64_REG_H0 && op->reg <= ARM64_REG_H31)
        {
          size_reg = 2;
        }
        else if((op->reg >= ARM64_REG_S0 && op->reg <= ARM64_REG_S31) || 
                (op->reg >= ARM64_REG_W0 && op->reg <= ARM64_REG_W30) ||
                (op->reg == ARM64_REG_WSP) || (op->reg == ARM64_REG_WZR))
        {
          size_reg = 4;
        }
        else if((op->reg >= ARM64_REG_D0 && op->reg <= ARM64_REG_D31) ||
                (op->reg >= ARM64_REG_X0 && op->reg <= ARM64_REG_X28) ||
                (op->reg == ARM64_REG_X29) || (op->reg == ARM64_REG_X30) ||
                (op->reg == ARM64_REG_XZR))
        {
          size_reg = 8;
        }
        else if ((op->reg >= ARM64_REG_Q0 && op->reg <= ARM64_REG_Q31) || 
                 (op->reg >= ARM64_REG_V0 && op->reg <= ARM64_REG_V31))
        {
          size_reg = 16;
        }
        break;
      }
    }
  }
  return size_reg;
}

unsigned int ARMDecoder::get_bytes_vess(unsigned int vec_elem)
{
  switch(vec_elem)
  {
    case ARM64_VESS_B: 
      return 1;
      
	case ARM64_VESS_H:
      return 2;
      
	case ARM64_VESS_S:
      return 4;
      
    case ARM64_VESS_D:
      return 8;
    
    default:
      return 0;
  }
}

unsigned int ARMDecoder::get_bytes_vas(unsigned int vec_elem)
{
  switch(vec_elem)
  {
    case ARM64_VAS_8B:
    case ARM64_VAS_16B:
      return 1;
      
    case ARM64_VAS_4H:
    case ARM64_VAS_8H:
      return 2;
      
    case ARM64_VAS_2S:
    case ARM64_VAS_4S:
      return 4;
      
    case ARM64_VAS_1D:
	case ARM64_VAS_2D:
      return 8;
      
	case ARM64_VAS_1Q:
      return 16;
    
    default:
      return 0;
  }
}

unsigned int ARMDecoder::get_simd_mem_bytes(const DecodedInst * inst)
{
  cs_insn* ci = ((ARMDecodedInst *)inst)->get_capstone_inst();
  cs_arm64 *arm64 = &(ci->detail->arm64);
  unsigned int accum_bytes = 0;  
  
  for (int i = 0; i < arm64->op_count; i++) 
  {
    cs_arm64_op *op = &(arm64->operands[i]);
    if (op->type == ARM64_OP_REG)
    {
      // loads/stores single structure - check the Vector size element
      if (op->vess != ARM64_VESS_INVALID) 
      {
        return this->get_bytes_vess(op->vess);
      }
      // loads/stores multiple structures - check the vector alignment  
      if (op->vas != ARM64_VAS_INVALID)
      {
        accum_bytes += this->get_bytes_vas(op->vas); 
      }
    }
  }
  return accum_bytes;
}

unsigned int ARMDecoder::size_mem_op (const DecodedInst * inst, unsigned int mem_idx)
{
  int inst_id = ((ARMDecodedInst *)inst)->get_capstone_inst()->id;
  if (m_arch == DL_ARCH_ARMv7)
  {
    return 4;  // TODO FIXME this is not correct, it depends on the instruction
  } else {  // AArch64
    switch (inst_id)
    {
      case ARM64_INS_LDARB:  // Byte
      case ARM64_INS_LDAXRB:
      case ARM64_INS_LDRB:
      case ARM64_INS_LDRSB:
      case ARM64_INS_LDTRB:
      case ARM64_INS_LDTRSB:
      case ARM64_INS_LDURB:
      case ARM64_INS_LDURSB:
      case ARM64_INS_LDXRB:
      case ARM64_INS_STLRB:
      case ARM64_INS_STLXRB:
      case ARM64_INS_STRB:
      case ARM64_INS_STTRB:
      case ARM64_INS_STURB:
      case ARM64_INS_STXRB:
        return 1;
        
      case ARM64_INS_LDARH:  // Half-word
      case ARM64_INS_LDAXRH:
      case ARM64_INS_LDRH:
      case ARM64_INS_LDRSH:
      case ARM64_INS_LDTRH:
      case ARM64_INS_LDTRSH:
      case ARM64_INS_LDURH:
      case ARM64_INS_LDURSH:
      case ARM64_INS_LDXRH:
      case ARM64_INS_STLRH:
      case ARM64_INS_STLXRH:
      case ARM64_INS_STRH:
      case ARM64_INS_STTRH:
      case ARM64_INS_STURH:
      case ARM64_INS_STXRH:
        return 2;
        
      case ARM64_INS_LDRSW:  // Word
      case ARM64_INS_LDTRSW:
      case ARM64_INS_LDURSW:
        return 4;
        
      case ARM64_INS_LDPSW: // Pair of words
        return 8;
      
      case ARM64_INS_LDAR: // depends on W (32) or X (64) register of other operands
      case ARM64_INS_LDAXR:
      case ARM64_INS_LDTR:
      case ARM64_INS_LDUR:
      case ARM64_INS_LDXR:
      case ARM64_INS_STLR:
      case ARM64_INS_STLXR:
      case ARM64_INS_STTR:
      case ARM64_INS_STUR:
      case ARM64_INS_STXP:
      case ARM64_INS_STXR:
      case ARM64_INS_LDAXP: // depends on W (32) or X (64) register of the other operands -- loads/stores pair, x2
      case ARM64_INS_LDXP:
      case ARM64_INS_STLXP:
      case ARM64_INS_LDNP: // depends on W, S (32), X, D (64) or Q (128) register of the other operands -- loads/stores pair, x2
      case ARM64_INS_LDP:
      case ARM64_INS_STNP:
      case ARM64_INS_STP:
      case ARM64_INS_LDR:  // depends on B (1), H (2), S/W (4), D/X (8) or Q (16) register of the other operand
      case ARM64_INS_STR:
        return this->size_mem_reg(inst);
        
      case ARM64_INS_LD1:  // SIMD registers, load 1 element
      case ARM64_INS_ST1:  // idem, store
      case ARM64_INS_LD1R: // SIMD load 1 element and replicate
        return this->get_simd_mem_bytes(inst);
   
      case ARM64_INS_LD2:  // SIMD registers, load 2 elements
      case ARM64_INS_ST2:
      case ARM64_INS_LD2R:
        return this->get_simd_mem_bytes(inst) * 2;

      case ARM64_INS_LD3:
      case ARM64_INS_ST3:
      case ARM64_INS_LD3R:
        return this->get_simd_mem_bytes(inst) * 3;

      case ARM64_INS_LD4:
      case ARM64_INS_ST4:
      case ARM64_INS_LD4R:
        return this->get_simd_mem_bytes(inst) * 4;

    } 
  }
  return 0;
}

unsigned int ARMDecoder::get_exec_microops(const DecodedInst *ins, int numLoads, int numStores)
{
  // TODO adapt to ARMv7
  unsigned int num_exec_uops = 1;  // default
  cs_insn* ci = ((ARMDecodedInst *)ins)->get_capstone_inst();
  //cs_arm64 * arm64 = &(ci->detail->arm64);
  int inst_id = ci->id;
  if (m_arch == DL_ARCH_ARMv8)
  {
    switch (inst_id)
    {
      case ARM64_INS_PRFM:  // Load and store instructions that do not use exec units
      case ARM64_INS_PRFUM:
      case ARM64_INS_LDTRSH:
      case ARM64_INS_LDTRB:
      case ARM64_INS_LDTRH:
      case ARM64_INS_LDTRSB:
      case ARM64_INS_LDTRSW:
      case ARM64_INS_LDTR:
      case ARM64_INS_LDURB:
      case ARM64_INS_LDUR:
      case ARM64_INS_LDURH:
      case ARM64_INS_LDURSB:
      case ARM64_INS_LDURSH:
      case ARM64_INS_LDURSW:
      case ARM64_INS_STTRB:
      case ARM64_INS_STTRH:
      case ARM64_INS_STTR:
      case ARM64_INS_STURB:
      case ARM64_INS_STUR:
      case ARM64_INS_STURH:
      case ARM64_INS_LDNP:
      case ARM64_INS_STNP:
      case ARM64_INS_LD1:  // FIXME the ASIMD ones are actually more complicated
        num_exec_uops = 0;
        break;
      case ARM64_INS_LDRB:  // Load and store instructions that may use exec units for writebacks
      case ARM64_INS_LDR:
      case ARM64_INS_LDRH:
      case ARM64_INS_LDRSB:
      case ARM64_INS_LDRSH:
      case ARM64_INS_LDRSW:
      case ARM64_INS_STRB:
      case ARM64_INS_STR:
      case ARM64_INS_STRH:
      case ARM64_INS_LDP:
      case ARM64_INS_LDPSW:
      case ARM64_INS_STP:
        //if (!(arm64->writeback)) //Most of the writeback operations are performed in parallel with the load
                                    //so there's no extra micro op
          num_exec_uops = 0;
        break;
/*      case ARM64_INS_ADD:  // Instructions that could have a shifter, so there are 2 uops
      case ARM64_INS_AND:
      case ARM64_INS_BIC:
      case ARM64_INS_EON:
      case ARM64_INS_EOR:
      case ARM64_INS_ORN:
      case ARM64_INS_ORR:
      case ARM64_INS_SUB:
        unsigned int n_ops = this->num_operands(ins);
        for(unsigned int idx = 0; idx < n_ops; ++idx)
        {
          if(arm64->operands[idx].shift.type != ARM64_SFT_INVALID)
          {
            num_exec_uops++;
            break;
          }
        }
        break;*/
    }
  }
  return num_exec_uops;
}

uint16_t ARMDecoder::get_operand_size(const DecodedInst *ins)
{
  if(m_arch == DL_ARCH_ARMv7)
  {
    return 32;  // TODO FIXME adapt to Thumb (16) and NEON
  } 
  else 
  {
    cs_insn* ci = ((ARMDecodedInst *)ins)->get_capstone_inst();
    cs_arm64 *arm64 = &(ci->detail->arm64);
    unsigned int max_size_reg = 0;
    for (int i = 0; i < arm64->op_count; i++) 
    {
      cs_arm64_op *op = &(arm64->operands[i]);
      if (op->type == ARM64_OP_REG)
      {
        if((max_size_reg < 8) && (op->reg >= ARM64_REG_B0 && op->reg <= ARM64_REG_B31))
        {
          max_size_reg = 8;
        }
        else if((max_size_reg < 16 ) && (op->reg >= ARM64_REG_H0 && op->reg <= ARM64_REG_H31))
        {
          max_size_reg = 16;
        }
        else if((max_size_reg < 32) && 
                ((op->reg >= ARM64_REG_S0 && op->reg <= ARM64_REG_S31) || 
                (op->reg >= ARM64_REG_W0 && op->reg <= ARM64_REG_W30) ||
                (op->reg == ARM64_REG_WSP) || (op->reg == ARM64_REG_WZR)))
        {
          max_size_reg = 32;
        }
        else if((max_size_reg < 64) && 
                ((op->reg >= ARM64_REG_D0 && op->reg <= ARM64_REG_D31) ||
                (op->reg >= ARM64_REG_X0 && op->reg <= ARM64_REG_X28) ||
                (op->reg == ARM64_REG_X29) || (op->reg == ARM64_REG_X30) ||
                (op->reg == ARM64_REG_XZR)))
        {
          max_size_reg = 64;
        }
        else if ((max_size_reg < 128) &&
                 ((op->reg >= ARM64_REG_Q0 && op->reg <= ARM64_REG_Q31) || 
                 (op->reg >= ARM64_REG_V0 && op->reg <= ARM64_REG_V31)))
        {
          max_size_reg = 128;
        }
      }
    }
    return max_size_reg;
  }
}

bool ARMDecoder::is_cache_flush_opcode(decoder_opcode opcd)
{
  if (m_arch == DL_ARCH_ARMv7)
  {
    return false;
  }
  else
  {
    // FIXME: more complicated than this, but then we would need to change the function signature.
    // Leaving it this way because nobody is using isCacheFlush right now.
    return opcd ==  ARM64_INS_DC;
  }
}

bool ARMDecoder::is_div_opcode(decoder_opcode opcd)
{
  if (m_arch == DL_ARCH_ARMv7)
  {
    return opcd == ARM_INS_SDIV 
      || opcd == ARM_INS_UDIV
      || opcd == ARM_INS_VDIV;
  }
  else
  {
    return opcd == ARM64_INS_SDIV 
      || opcd == ARM64_INS_UDIV
      || opcd == ARM64_INS_FDIV;
  }
}

bool ARMDecoder::is_pause_opcode(decoder_opcode opcd)
{
  return false;
}

bool ARMDecoder::is_branch_opcode(decoder_opcode opcd)
{
  if (m_arch == DL_ARCH_ARMv7)
  {
    return ((opcd >= ARM_INS_BL && opcd <= ARM_INS_B) || (opcd >= ARM_INS_TBB && opcd <= ARM_INS_CBZ));
  }
  else
  {
    return ((opcd == ARM64_INS_B) || 
            (opcd >= ARM64_INS_BL && opcd <= ARM64_INS_BR) || 
            (opcd >= ARM64_INS_CBNZ && opcd <= ARM64_INS_CBZ) ||
            (opcd == ARM64_INS_TBNZ) || (opcd == ARM64_INS_TBZ) ||
            (opcd == ARM64_INS_RET));
  }
}

bool ARMDecoder::is_fpvector_addsub_opcode(decoder_opcode opcd, const DecodedInst* ins) 
{
  bool is_vas = false;
  if (m_arch == DL_ARCH_ARMv7)
  {
    switch(opcd)
    {
      case ARM_INS_VADD:
      case ARM_INS_VADDHN:
      case ARM_INS_VADDL:
      case ARM_INS_VADDW:
      case ARM_INS_VHADD:
      case ARM_INS_VHSUB:
      case ARM_INS_VPADDL:
      case ARM_INS_VPADD:
      case ARM_INS_VQADD:
      case ARM_INS_VRADDHN:
      case ARM_INS_VRHADD:
      case ARM_INS_VQSUB:
      case ARM_INS_VRSUBHN:
      case ARM_INS_VSUB:
      case ARM_INS_VSUBHN:
      case ARM_INS_VSUBL:
      case ARM_INS_VSUBW:
        is_vas = true;
        break;
      default:
        break;
    }
  }
  else
  {
    switch(opcd)
    {
      case ARM64_INS_ADDHN:
      case ARM64_INS_ADDHN2:
      case ARM64_INS_ADDP:
      case ARM64_INS_ADDV:
      case ARM64_INS_FADD:
      case ARM64_INS_FADDP:
      case ARM64_INS_RADDHN:
      case ARM64_INS_RADDHN2:
      case ARM64_INS_SADDLP:
      case ARM64_INS_SADDLV:
      case ARM64_INS_SADDL2:
      case ARM64_INS_SADDL:
      case ARM64_INS_SADDW2:
      case ARM64_INS_SADDW:
      case ARM64_INS_SHADD:
      case ARM64_INS_SQADD:
      case ARM64_INS_SRHADD:
      case ARM64_INS_SUQADD:
      case ARM64_INS_UADDLP:
      case ARM64_INS_UADDLV:
      case ARM64_INS_UADDL2:
      case ARM64_INS_UADDL:
      case ARM64_INS_UADDW2:
      case ARM64_INS_UADDW:
      case ARM64_INS_UHADD:
      case ARM64_INS_URHADD:
      case ARM64_INS_USQADD:
      case ARM64_INS_FSUB:
      case ARM64_INS_RSUBHN:
      case ARM64_INS_RSUBHN2:
      case ARM64_INS_SHSUB:
      case ARM64_INS_SQSUB:
      case ARM64_INS_SSUBL2:
      case ARM64_INS_SSUBL:
      case ARM64_INS_SSUBW2:
      case ARM64_INS_SSUBW:
      case ARM64_INS_SUBHN:
      case ARM64_INS_SUBHN2:
      case ARM64_INS_UHSUB:
      case ARM64_INS_UQSUB:
      case ARM64_INS_USUBL2:
      case ARM64_INS_USUBL:
      case ARM64_INS_USUBW2:
      case ARM64_INS_USUBW:
      case ARM64_INS_FABD:
      case ARM64_INS_FABS:
      case ARM64_INS_FACGE:
      case ARM64_INS_FACGT:
      case ARM64_INS_FCMEQ:
      case ARM64_INS_FCMGE:
      case ARM64_INS_FCMGT:
      case ARM64_INS_FCMLE:
      case ARM64_INS_FCMLT:
      case ARM64_INS_SCVTF:
      case ARM64_INS_UCVTF:
      case ARM64_INS_FMAX:
      case ARM64_INS_FMAXNM:
      case ARM64_INS_FMAXNMP:
      case ARM64_INS_FMAXNMV:
      case ARM64_INS_FMAXP:
      case ARM64_INS_FMAXV:
      case ARM64_INS_FMIN:
      case ARM64_INS_FMINNM:
      case ARM64_INS_FMINNMP:
      case ARM64_INS_FMINNMV:
      case ARM64_INS_FMINP:
      case ARM64_INS_FMINV:
      case ARM64_INS_FNEG:
      case ARM64_INS_FRINTA:
      case ARM64_INS_FRINTI:
      case ARM64_INS_FRINTM:
      case ARM64_INS_FRINTN:
      case ARM64_INS_FRINTP:
      case ARM64_INS_FRINTX:
      case ARM64_INS_FRINTZ:
      case ARM64_INS_FMOV:
        is_vas = true;
        break;
      case ARM64_INS_ADD:  // Can be vector or scalar -- check registers
      case ARM64_INS_SUB:
        if (this->get_operand_size(ins) == 128)  // we're using vector registers
          is_vas = true;
        break;
      default:
        break;
    }
  }
  
  return is_vas;
}

bool ARMDecoder::is_fpvector_muldiv_opcode(decoder_opcode opcd, const DecodedInst* ins) 
{
  bool is_vmd = false;
  
  if (m_arch == DL_ARCH_ARMv7)
  {
    switch(opcd)
    { 
      case ARM_INS_VMUL:
      case ARM_INS_VMULL:
      case ARM_INS_VNMUL:
      case ARM_INS_VQDMULH:
      case ARM_INS_VQDMULL:
      case ARM_INS_VQRDMULH:
      case ARM_INS_VDIV:
        is_vmd = true;
        break;
      default:
        break;
    }
  }
  else  // ARMv8
  {
    switch(opcd)
    {
      case ARM64_INS_FMUL:
      case ARM64_INS_FMULX:
      case ARM64_INS_FNMUL:
      case ARM64_INS_PMULL2:
      case ARM64_INS_PMULL:
      case ARM64_INS_PMUL:
      case ARM64_INS_SQDMULH:
      case ARM64_INS_SQDMULL:
      case ARM64_INS_SQDMULL2:
      case ARM64_INS_SQRDMULH:
      case ARM64_INS_UMSUBL:
      case ARM64_INS_UMULL2:
      case ARM64_INS_FMADD:
      case ARM64_INS_FNMADD:
      case ARM64_INS_FMSUB:
      case ARM64_INS_FNMSUB:
      case ARM64_INS_FDIV:
      case ARM64_INS_FMLA:
      case ARM64_INS_FMLS:
      case ARM64_INS_FCVTAS:
      case ARM64_INS_FCVTAU:
      case ARM64_INS_FCVTMS:
      case ARM64_INS_FCVTMU:
      case ARM64_INS_FCVTNS:
      case ARM64_INS_FCVTNU:
      case ARM64_INS_FCVTPS:
      case ARM64_INS_FCVTPU:
      case ARM64_INS_FCVTZS:
      case ARM64_INS_FCVTZU:
      case ARM64_INS_FCVT:
      case ARM64_INS_FCVTL:
      case ARM64_INS_FCVTL2:
      case ARM64_INS_FCVTN:
      case ARM64_INS_FCVTN2:
      case ARM64_INS_FCVTXN:
      case ARM64_INS_FCVTXN2:
        is_vmd = true;
        break;
      case ARM64_INS_MUL:  // can be scalar or vector: check registers used
      case ARM64_INS_UMULL:
        if (this->get_operand_size(ins) == 128)  // we're using vector registers
          is_vmd = true;
        break;
      default:
        break;
    }
  }
  
  return is_vmd;
}

bool ARMDecoder::is_fpvector_ldst_opcode(decoder_opcode opcd, const DecodedInst* ins)
{
  bool is_vls = false;

  if (m_arch == DL_ARCH_ARMv7)
  {
    switch(opcd)
    {
      case ARM_INS_FLDMDBX:
      case ARM_INS_FLDMIAX:
      case ARM_INS_FSTMDBX:
      case ARM_INS_FSTMIAX:
      case ARM_INS_VLD1:
      case ARM_INS_VLD2:
      case ARM_INS_VLD3:
      case ARM_INS_VLD4:
      case ARM_INS_VLDMDB:
      case ARM_INS_VLDMIA:
      case ARM_INS_VLDR:
      case ARM_INS_VST1:
      case ARM_INS_VST2:
      case ARM_INS_VST3:
      case ARM_INS_VST4:
      case ARM_INS_VSTMDB:
      case ARM_INS_VSTMIA:
      case ARM_INS_VSTR:
        is_vls = true;
        break;
      default:
        break;
    }
  }
  else
  {
    switch (opcd)
    {
      case ARM64_INS_LD1:
      case ARM64_INS_ST1:
      case ARM64_INS_LD1R:
      case ARM64_INS_LD2:
      case ARM64_INS_ST2:
      case ARM64_INS_LD2R:
      case ARM64_INS_LD3:
      case ARM64_INS_ST3:
      case ARM64_INS_LD3R:
      case ARM64_INS_LD4:
      case ARM64_INS_ST4:
      case ARM64_INS_LD4R:
        is_vls = true;
        break;
      case ARM64_INS_LDNP: // scalar or vector? depends on registers used
      case ARM64_INS_LDP:
      case ARM64_INS_STNP:
      case ARM64_INS_STP:
      case ARM64_INS_LDR:  
      case ARM64_INS_STR:
        if (this->get_operand_size(ins) == 128)  // we're using vector registers
          is_vls = true;
        break;
      default:
        break;        

    } 
  }

  return is_vls;
}

Decoder::decoder_reg ARMDecoder::last_reg()
{
  if (m_arch == DL_ARCH_ARMv7)
  {
    return ARM_REG_ENDING;
  }
  else
  {
    return ARM64_REG_ENDING;
  }
}

uint32_t ARMDecoder::map_register(decoder_reg reg) {
    int offset = 0;
    if (reg >= ARM64_REG_X0 && reg <= ARM64_REG_X28) {
        offset = -31;
    }
    else if (reg >= ARM64_REG_X29 && reg <= ARM64_REG_X30) {
        offset = 196;
    }
    else if (reg >= ARM64_REG_V0 && reg <= ARM64_REG_V31) {
        offset = -220;
    }
    else if (reg >= ARM64_REG_S0 && reg <= ARM64_REG_S31) {
        offset = -128;
    }
    else if (reg >= ARM64_REG_Q0 && reg <= ARM64_REG_Q31) {
        offset = -96;
    }
    else if (reg >= ARM64_REG_H0 && reg <= ARM64_REG_H31) {
        offset = -64;
    }
    else if (reg >= ARM64_REG_D0 && reg <= ARM64_REG_D31) {
        offset = -32;
    }
    return reg + offset;
}

unsigned int ARMDecoder::num_read_implicit_registers(const DecodedInst *inst) {
    cs_insn * csi = ((ARMDecodedInst *)inst)->get_capstone_inst();

    switch (csi->id) {
    //Capstone bug (really?): Ret instruction reads from x30 if register operand is omitted
    case ARM64_INS_RET:
       return num_operands(inst) ? 0:1;
    }

    cs_detail *detail = csi->detail;

    return detail->regs_read_count;
}
Decoder::decoder_reg ARMDecoder::get_read_implicit_reg(const DecodedInst* inst, unsigned int idx) {
    cs_insn * csi = ((ARMDecodedInst *)inst)->get_capstone_inst();
    cs_detail *detail = csi->detail;

    switch (csi->id) {
    //Capstone bug (really?): Ret instruction reads from x30 if register is omitted
    case ARM64_INS_RET:
       return num_operands(inst) ? ARM64_REG_INVALID:ARM64_REG_X30;
    }

    return detail->regs_read[idx];
}
unsigned int ARMDecoder::num_write_implicit_registers(const DecodedInst *inst) {
    cs_insn * csi = ((ARMDecodedInst *)inst)->get_capstone_inst();
    cs_detail *detail = csi->detail;

    return detail->regs_write_count;
}
Decoder::decoder_reg ARMDecoder::get_write_implicit_reg(const DecodedInst *inst, unsigned int idx) {
    cs_insn * csi = ((ARMDecodedInst *)inst)->get_capstone_inst();
    cs_detail *detail = csi->detail;

    return detail->regs_write[idx];
}

ARMDecodedInst::ARMDecodedInst(Decoder* d, const uint8_t * code, size_t size, uint64_t address)
{
  this->m_dec = d;
  this->m_code = code;
  this->m_size = size;
  this->m_address = address;
  this->m_already_decoded = false;
  this->capstone_inst = cs_malloc(((ARMDecoder *)d)->get_handle());
}

ARMDecodedInst::~ARMDecodedInst()
{
  cs_free(this->capstone_inst, 1);
}

cs_insn * ARMDecodedInst::get_capstone_inst()
{
  return this->capstone_inst;
}

unsigned int ARMDecodedInst::inst_num_id() const
{
  return (this->capstone_inst->id);
}

void ARMDecodedInst::set_disassembly()
{
  char* temp_operands = this->capstone_inst->op_str;
  char* temp_mnemonic = this->capstone_inst->mnemonic;

  m_disassembly.assign(temp_mnemonic);
  m_disassembly += " ";
  m_disassembly += temp_operands;
}

std::string ARMDecodedInst::disassembly_to_str() const
{
  return this->m_disassembly;
}

bool ARMDecodedInst::is_nop() const
{
  if (m_dec->get_arch() == DL_ARCH_ARMv7)
  {
    return (this->capstone_inst->id == ARM_INS_NOP);
  }
  else
  {
    return (this->capstone_inst->id == ARM64_INS_NOP);
  }  
}

bool ARMDecodedInst::is_atomic() const
{
  if (m_dec->get_arch() == DL_ARCH_ARMv7)
  {
    return false;  // FIXME: Not sure how to get this...
  } 
  else  
  {
    // Get instruction Id
    unsigned int ins_id = this->capstone_inst->id;
    return ((ins_id >= ARM64_INS_LDXP && ins_id <= ARM64_INS_LDXR) ||
            (ins_id >= ARM64_INS_LDXP && ins_id <= ARM64_INS_LDXR) ||
            (ins_id >= ARM64_INS_STLXP && ins_id <= ARM64_INS_STLXR) ||
            (ins_id >= ARM64_INS_STXP && ins_id <= ARM64_INS_STXR));
  }
}

bool ARMDecodedInst::is_prefetch() const
{
  if (m_dec->get_arch() == DL_ARCH_ARMv7)
  {
    return false;  
  } 
  else  
  {
    // Get instruction Id
    unsigned int ins_id = this->capstone_inst->id;
    return (ins_id == ARM64_INS_PRFM || ins_id == ARM64_INS_PRFUM);
  }
}

bool ARMDecodedInst::is_serializing() const
{
  bool is_serializing = false;
  
  if (m_dec->get_arch() == DL_ARCH_ARMv7)
  {
    // Get instruction Id
    unsigned int ins_id = this->capstone_inst->id;
  
    // TODO case MCR cp14/cp15 and MRC p14 -- capstone does not identify coprocessor registers
    // TODO "prefetch abort handler" and "undef. instr. exception hadler" also serializing
    // First evaluate special cases
    if (ins_id == ARM_INS_MSR)
    {
      cs_arm * arm = &(capstone_inst->detail->arm);
      int count = arm->op_count;
      for (int i = 0; i < count; i++) {
        cs_arm_op *op = &(arm->operands[i]);
        if(op->type == ARM_OP_SYSREG) 
        {
          switch (op->reg)
          {
            case ARM_SYSREG_SPSR_C:  // MSR SPSR
            case ARM_SYSREG_SPSR_X:
            case ARM_SYSREG_SPSR_S:
            case ARM_SYSREG_SPSR_F:
            case ARM_SYSREG_CPSR_C:  // MSR CPSR of control bits
              is_serializing = true;
              break;
          }
        }
      }
    } 
    /*else if (ins_id == ARM_INS_MOVS || ins_id == ARM_INS_SUBS)
    {
      // data processing instruction operating on PC register with the S bit set 
      cs_arm * arm = &(capstone_inst->detail->arm);
      int count = arm->op_count;
      for (int i = 0; i < count; i++) {
        cs_arm_op *op = &(arm->operands[i]);
        if(op->type == ARM_OP_REG && op->reg == ARM_REG_PC) 
          is_serializing = true;
      }
    } */
    else if (ins_id == ARM_INS_LDM)
    {
      // LDM pc ^
      cs_arm * arm = &(capstone_inst->detail->arm);
      int count = arm->op_count;
      for (int i = 0; i < count; i++) {
        cs_arm_op *op = &(arm->operands[i]);
        if(op->type == ARM_OP_REG && op->reg == ARM_REG_PC && arm->update_flags == true) 
          is_serializing = true;
      }
    }
    // If not part of the special cases, check if group of serializing instructions
    else {
      switch(ins_id)
      {
        case ARM_INS_SVC:
        case ARM_INS_SMC:
        case ARM_INS_BKPT:
        case ARM_INS_CPS:
        case ARM_INS_SETEND:
        case ARM_INS_RFEDA:
        case ARM_INS_RFEDB:
        case ARM_INS_RFEIA:
        case ARM_INS_RFEIB:
        case ARM_INS_WFE:
        case ARM_INS_WFI:
        case ARM_INS_SEV:
        case ARM_INS_CLREX:
        case ARM_INS_DSB:
          is_serializing = true;
          break;
        default:
          is_serializing = false;
          break;
      }
    }
  } 
  // TODO for armv8 no information found!!
  
  return is_serializing;
}

bool ARMDecodedInst::is_conditional_branch() const
{
  bool is_cb = false;
  if (m_dec->get_arch() == DL_ARCH_ARMv7)
  {
    // Get instruction Id
    unsigned int ins_id = this->capstone_inst->id;
  
    // Instructions that are always a conditional branch
    if (ins_id >= ARM_INS_CBNZ && ins_id <= ARM_INS_CBZ)
    {
      is_cb = true;
    }
    else if(ins_id >= ARM_INS_BL && ins_id <= ARM_INS_B)  // We have to check the condition codes
    {
      cs_arm * arm = &(capstone_inst->detail->arm);
      arm_cc cc = arm->cc;
      if (cc!=ARM_CC_INVALID && cc!=ARM_CC_AL)  // AL = always/unconditional
        is_cb = true;
    }
  } 
  else  // ARMv8
  {
    // Get instruction Id
    unsigned int ins_id = this->capstone_inst->id;
  
    // Instructions that are always a conditional branch
    if ((ins_id >= ARM64_INS_CBNZ && ins_id <= ARM64_INS_CBZ) || ins_id == ARM64_INS_TBZ || ins_id == ARM64_INS_TBNZ)
    {
      is_cb = true;
    }
    else if(ins_id == ARM64_INS_B)  // We have to check the condition codes
    {
      cs_arm64 * arm64 = &(capstone_inst->detail->arm64);
      arm64_cc cc = arm64->cc;
      if (cc != ARM64_CC_INVALID && cc != ARM64_CC_AL && cc != ARM64_CC_NV)  // AL/NV = always/unconditional
        is_cb = true;
    }
  }

  return is_cb;
}

bool ARMDecodedInst::is_indirect_branch() const
{
  bool is_ib = false;
  if (m_dec->get_arch() == DL_ARCH_ARMv7)
  {
    // Get instruction Id
    unsigned int ins_id = this->capstone_inst->id;
  
    // Instructions that are always a conditional branch
    if (ins_id == ARM_INS_B)
    {
      is_ib = true;
    }
  } 
  else  // ARMv8
  {
    // Get instruction Id
    unsigned int opcd = this->capstone_inst->id;
  
    // Instructions that are always a conditional branch
    if (opcd == ARM64_INS_BR || opcd == ARM64_INS_BLR)
    {
      is_ib = true;
    }
  }

  return is_ib;
}

bool ARMDecodedInst::is_barrier() const 
{
  unsigned int ins_id = this->capstone_inst->id;
  if (m_dec->get_arch() == DL_ARCH_ARMv7)
  {
    return (ins_id == ARM_INS_DMB);
  } 
  else 
  {
    return (ins_id == ARM64_INS_ISB || ins_id == ARM64_INS_DMB || ins_id == ARM64_INS_DSB);
  }
}

bool ARMDecodedInst::src_dst_merge() const
{
  return false;
}

bool ARMDecodedInst::is_X87() const
{
  return false;
}

bool ARMDecodedInst::has_modifiers() const
{
  cs_arm64 * arm64 = &(capstone_inst->detail->arm64);
  int count = arm64->op_count;
  for (int i = 0; i < count; i++) 
  {
    cs_arm64_op *op = &(arm64->operands[i]);
    if(op->shift.type != ARM64_SFT_INVALID || op->ext != ARM64_EXT_INVALID) 
      return true;
  }
  return false;
}

bool ARMDecodedInst::is_mem_pair() const
{
  unsigned int ins_id = this->capstone_inst->id;
  if (m_dec->get_arch() == DL_ARCH_ARMv7)
  {
    return false;  // TODO
  } 
  else  // DL_ARCH_ARMv8
  {
    return ((ins_id >= ARM64_INS_LDNP && ins_id <= ARM64_INS_LDPSW) ||
            (ins_id == ARM64_INS_STNP) || (ins_id == ARM64_INS_STP) );
  } 
}

bool ARMDecodedInst::is_writeback() const {
    if (m_dec->get_arch() == DL_ARCH_ARMv8) {
        return capstone_inst->detail->arm64.writeback;
    }
    return capstone_inst->detail->arm.writeback;
}

} // namespace dl;
