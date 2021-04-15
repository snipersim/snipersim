#include "x86_decoder.h"
#include <iostream>

extern "C" 
{
  #include <xed-iclass-enum.h>
  #include <xed-reg-class.h>  
  #include <xed-interface.h>
}

namespace dl 
{

X86Decoder::X86Decoder(dl_arch arch, dl_mode mode, dl_syntax syntax)
{
  xed_state_t init;
  
  xed_tables_init();      
  
  switch(mode)
  {
    case DL_MODE_32:
      init = { XED_MACHINE_MODE_LONG_COMPAT_32, XED_ADDRESS_WIDTH_32b };
      break;
    case DL_MODE_64:
    default:
      init = { XED_MACHINE_MODE_LONG_64, XED_ADDRESS_WIDTH_64b };
  }
  
  m_xed_state_init = init;

  this->m_arch = arch;
  this->m_mode = mode;
  this->m_syntax = syntax;
  if (mode == DL_MODE_32)
    this->m_isa = DL_ISA_IA32;
  else
    this->m_isa = DL_ISA_X86_64;
}

X86Decoder::~X86Decoder()
{
}

void X86Decoder::decode(DecodedInst * inst)
{  
  xed_state_t xed_state;
  xed_error_enum_t res_decode;
  xed_decoded_inst_t* xi;
  
  if(inst->get_already_decoded())
    return;
  
  xed_state = m_xed_state_init;
  xi = ((X86DecodedInst *)inst)->get_xed_inst();
  xed_decoded_inst_zero_set_mode(const_cast<xed_decoded_inst_t*>(xi), &xed_state);
  res_decode = xed_decode(const_cast<xed_decoded_inst_t*>(xi), inst->get_code(), inst->get_size());
  assert(res_decode == XED_ERROR_NONE);

  inst->set_already_decoded(true);
}

void X86Decoder::decode(DecodedInst * inst, dl_isa isa)
{
  this->decode(inst);
}

// This function has no real effect for XED, because the initialization is already done
void X86Decoder::change_isa_mode(dl_isa new_isa)
{
  this->m_isa = new_isa;
}
 
const char* X86Decoder::inst_name(unsigned int inst_id)
{
  return xed_iclass_enum_t2str(static_cast<xed_iclass_enum_t>(inst_id));
}
 
const char* X86Decoder::reg_name(unsigned int reg_id)
{
  return xed_reg_enum_t2str(static_cast<xed_reg_enum_t>(reg_id));
}

Decoder::decoder_reg X86Decoder::largest_enclosing_register(Decoder::decoder_reg r)
{
  return xed_get_largest_enclosing_register(static_cast<xed_reg_enum_t>(r));
}

bool X86Decoder::invalid_register(decoder_reg r)
{
  return r == XED_REG_INVALID;
}

bool X86Decoder::reg_is_program_counter(decoder_reg r)
{
  return (r == XED_REG_EIP || r == XED_REG_RIP);
}

bool X86Decoder::inst_in_group(const DecodedInst * inst, unsigned int group_id)
{
  return (xed_decoded_inst_get_category(((X86DecodedInst *)inst)->get_xed_inst()) == group_id);
}

unsigned int X86Decoder::num_memory_operands(const DecodedInst * inst)
{
  return xed_decoded_inst_number_of_memory_operands(
              static_cast<const xed_decoded_inst_t*>(((X86DecodedInst *)inst)->get_xed_inst()));
}

unsigned int X86Decoder::num_operands(const DecodedInst * inst)
{   
  const xed_inst_t *i = xed_decoded_inst_inst(
              static_cast<const xed_decoded_inst_t*>(((X86DecodedInst *)inst)->get_xed_inst()));

  return xed_inst_noperands(i);
}

Decoder::decoder_reg X86Decoder::mem_base_reg (const DecodedInst * inst, unsigned int mem_idx) 
{
  return xed_decoded_inst_get_base_reg(
        static_cast<const xed_decoded_inst_t*>(((X86DecodedInst *)inst)->get_xed_inst()), mem_idx);
}

Decoder::decoder_reg X86Decoder::mem_index_reg (const DecodedInst * inst, unsigned int mem_idx) 
{
  return xed_decoded_inst_get_index_reg(
        static_cast<const xed_decoded_inst_t*>(((X86DecodedInst *)inst)->get_xed_inst()), mem_idx);
}

bool X86Decoder::op_read_mem(const DecodedInst * inst, unsigned int mem_idx)
{
  return xed_decoded_inst_mem_read(
        static_cast<const xed_decoded_inst_t*>(((X86DecodedInst *)inst)->get_xed_inst()), mem_idx);
}

bool X86Decoder::op_write_mem(const DecodedInst * inst, unsigned int mem_idx)
{
  return xed_decoded_inst_mem_written(
        static_cast<const xed_decoded_inst_t*>(((X86DecodedInst *)inst)->get_xed_inst()), mem_idx);
}

bool X86Decoder::op_read_reg (const DecodedInst * inst, unsigned int idx)
{
  const xed_operand_t *op = get_operand(inst, idx);
  
  return xed_operand_read(op);
}

bool X86Decoder::op_write_reg (const DecodedInst * inst, unsigned int idx)
{
  const xed_operand_t *op = get_operand(inst, idx);
  
  return xed_operand_written(op);
}

// Private function
const xed_operand_t * X86Decoder::get_operand(const DecodedInst * inst, unsigned int idx)
{
  const xed_inst_t *i = xed_decoded_inst_inst(
        static_cast<const xed_decoded_inst_t*>(((X86DecodedInst *)inst)->get_xed_inst()));

  return xed_inst_operand(i, idx);
}

// Private function
xed_operand_enum_t X86Decoder::get_operand_name(const DecodedInst * inst, unsigned int idx)
{
  const xed_operand_t *op = get_operand(inst, idx);
  
  return xed_operand_name(op);
}

bool X86Decoder::is_addr_gen(const DecodedInst * inst, unsigned int idx)
{
  xed_operand_enum_t name = get_operand_name(inst, idx);
  
  return (name == XED_OPERAND_AGEN);
}

bool X86Decoder::op_is_reg (const DecodedInst * inst, unsigned int idx)
{
  xed_operand_enum_t name = get_operand_name(inst, idx);

  return (xed_operand_is_register(name));
}

Decoder::decoder_reg X86Decoder::get_op_reg (const DecodedInst * inst, unsigned int idx)
{
  xed_operand_enum_t name = get_operand_name(inst, idx);

  return xed_decoded_inst_get_reg(
        static_cast<const xed_decoded_inst_t*>(((X86DecodedInst *)inst)->get_xed_inst()), name);
}


unsigned int X86Decoder::size_mem_op (const DecodedInst * inst, unsigned int mem_idx)
{
  return xed_decoded_inst_get_memory_operand_length (
        static_cast<const xed_decoded_inst_t*>(((X86DecodedInst *)inst)->get_xed_inst()), mem_idx);
}

unsigned int X86Decoder::get_exec_microops(const DecodedInst *ins, int numLoads, int numStores)
{
  xed_decoded_inst_t * ins_xed = ((X86DecodedInst *)ins)->get_xed_inst();
  if (xed_decoded_inst_get_category(ins_xed) == XED_CATEGORY_DATAXFER 
      || xed_decoded_inst_get_category(ins_xed) == XED_CATEGORY_CMOV 
      || xed_decoded_inst_get_iclass(ins_xed) == XED_ICLASS_PUSH 
      || xed_decoded_inst_get_iclass(ins_xed) == XED_ICLASS_POP)
  {
    unsigned int numExecs = 0;

    // Move instructions with additional microops to process the load and store information
    switch(xed_decoded_inst_get_iclass(ins_xed))
    {
      case XED_ICLASS_MOVLPS:
      case XED_ICLASS_MOVLPD:
      case XED_ICLASS_MOVHPS:
      case XED_ICLASS_MOVHPD:
      case XED_ICLASS_SHUFPS:
      case XED_ICLASS_SHUFPD:
      case XED_ICLASS_BLENDPS:
      case XED_ICLASS_BLENDPD:
      case XED_ICLASS_EXTRACTPS:
      case XED_ICLASS_ROUNDSS:
      case XED_ICLASS_ROUNDPS:
      case XED_ICLASS_ROUNDSD:
      case XED_ICLASS_ROUNDPD:
        numExecs += 1;
        break;
      case XED_ICLASS_INSERTPS:
        numExecs += 2;
        break;
      default:
        break;
    }

    // Explicit register moves. Normal loads and stores do not require this.
    if ((numLoads + numStores) == 0)
    {
      numExecs += 1;
    }

    return numExecs;
  }
  else
  {
    return 1;
  }
}

uint16_t X86Decoder::get_operand_size(const DecodedInst *ins)
{
  uint16_t operand_size = 0;
    
  const xed_inst_t *inst = xed_decoded_inst_inst(
        static_cast<const xed_decoded_inst_t*>(((X86DecodedInst *)ins)->get_xed_inst()));
  for(uint32_t idx = 0; idx < xed_inst_noperands(inst); ++idx)
  {
    const xed_operand_t *op = xed_inst_operand(inst, idx);
    xed_operand_enum_t name = xed_operand_name(op);

    if (xed_operand_is_register(name))
    {
      xed_reg_enum_t reg = xed_decoded_inst_get_reg(
            static_cast<const xed_decoded_inst_t*>(((X86DecodedInst *)ins)->get_xed_inst()), name);

      switch(reg)
      {
        case XED_REG_RFLAGS:
        case XED_REG_RIP:
        case XED_REG_RSP:
          continue;
        default:
          ;
      }
    }
    operand_size = std::max(operand_size, (uint16_t)xed_decoded_inst_get_operand_width(
          static_cast<const xed_decoded_inst_t*>(((X86DecodedInst *)ins)->get_xed_inst()))); 
  }
  if (operand_size == 0)
    operand_size = 64;
      
  return operand_size;
}

bool X86Decoder::is_cache_flush_opcode(decoder_opcode opcd) 
{
  // FIXME (from Sniper): Old decoder only listed these three instructions, but there may be more (INVEPT, INVLPGA, INVVPID)
  //        Currently, no-one is using isCacheFlush though.
  return opcd == XED_ICLASS_WBINVD
      || opcd == XED_ICLASS_INVD
      || opcd == XED_ICLASS_INVLPG;
}

bool X86Decoder::is_div_opcode(decoder_opcode opcd) 
{
  return opcd == XED_ICLASS_DIV || opcd == XED_ICLASS_IDIV;
}

bool X86Decoder::is_pause_opcode(decoder_opcode opcd) 
{
  return opcd == XED_ICLASS_PAUSE;
}

bool X86Decoder::is_branch_opcode(decoder_opcode opcd) 
{
  bool is_b = false;
  
  switch(opcd)
  {
    case XED_ICLASS_CALL_FAR:
    case XED_ICLASS_CALL_NEAR:
    case XED_ICLASS_JB:
    case XED_ICLASS_JBE:
    case XED_ICLASS_JL:
    case XED_ICLASS_JLE:
    case XED_ICLASS_JMP:
    case XED_ICLASS_JMP_FAR:
    case XED_ICLASS_JNB:
    case XED_ICLASS_JNBE:
    case XED_ICLASS_JNL:
    case XED_ICLASS_JNLE:
    case XED_ICLASS_JNO:
    case XED_ICLASS_JNP:
    case XED_ICLASS_JNS:
    case XED_ICLASS_JNZ:
    case XED_ICLASS_JO:
    case XED_ICLASS_JP:
    case XED_ICLASS_JRCXZ:
    case XED_ICLASS_JS:
    case XED_ICLASS_JZ:
    case XED_ICLASS_RET_FAR:
    case XED_ICLASS_RET_NEAR:
      is_b = true;
      break;
    default:
      break;
  }
  
  return is_b;
}

bool X86Decoder::is_fpvector_addsub_opcode(decoder_opcode opcd, const DecodedInst* ins)
{
  bool is_vas = false;
  
  switch(opcd)
  {
    case XED_ICLASS_ADDPD:
    case XED_ICLASS_ADDPS:
    case XED_ICLASS_ADDSD:
    case XED_ICLASS_ADDSS:
    case XED_ICLASS_ADDSUBPD:
    case XED_ICLASS_ADDSUBPS:
    case XED_ICLASS_SUBPD:
    case XED_ICLASS_SUBPS:
    case XED_ICLASS_SUBSD:
    case XED_ICLASS_SUBSS:
    case XED_ICLASS_VADDPD:
    case XED_ICLASS_VADDPS:
    case XED_ICLASS_VADDSD:
    case XED_ICLASS_VADDSS:
    case XED_ICLASS_VADDSUBPD:
    case XED_ICLASS_VADDSUBPS:
    case XED_ICLASS_VSUBPD:
    case XED_ICLASS_VSUBPS:
    case XED_ICLASS_VSUBSD:
    case XED_ICLASS_VSUBSS:
       is_vas = true;
      break;
    default:
      break;
  }
  
  return is_vas;
}

bool X86Decoder::is_fpvector_muldiv_opcode(decoder_opcode opcd, const DecodedInst* ins)
{
  bool is_vmd = false;
  
  switch(opcd)
  {      
    case XED_ICLASS_MULPD:
    case XED_ICLASS_MULPS:
    case XED_ICLASS_MULSD:
    case XED_ICLASS_MULSS:
    case XED_ICLASS_DIVPD:
    case XED_ICLASS_DIVPS:
    case XED_ICLASS_DIVSD:
    case XED_ICLASS_DIVSS:
    case XED_ICLASS_VMULPD:
    case XED_ICLASS_VMULPS:
    case XED_ICLASS_VMULSD:
    case XED_ICLASS_VMULSS:
    case XED_ICLASS_VDIVPD:
    case XED_ICLASS_VDIVPS:
    case XED_ICLASS_VDIVSD:
    case XED_ICLASS_VDIVSS:       
      is_vmd = true;
      break;
    default:
      break;
  }
  
  return is_vmd;
}

bool X86Decoder::is_fpvector_ldst_opcode(decoder_opcode opcd, const DecodedInst* ins)
{
  bool is_vls = false;

  switch(opcd)
  {
    case XED_ICLASS_MOVAPS:
    case XED_ICLASS_MOVAPD:
    case XED_ICLASS_MOVUPS:
    case XED_ICLASS_MOVUPD:
    case XED_ICLASS_MOVSS:
    case XED_ICLASS_MOVSD_XMM:
    case XED_ICLASS_MOVHPS:
    case XED_ICLASS_MOVHPD:
    case XED_ICLASS_MOVLPS:
    case XED_ICLASS_MOVLPD:
    case XED_ICLASS_SHUFPS:
    case XED_ICLASS_SHUFPD:
    case XED_ICLASS_BLENDPS:
    case XED_ICLASS_BLENDPD:
    case XED_ICLASS_MOVDDUP:
    case XED_ICLASS_MOVSHDUP:
    case XED_ICLASS_MOVSLDUP:
    case XED_ICLASS_UNPCKHPS:
    case XED_ICLASS_UNPCKLPS:
    case XED_ICLASS_UNPCKHPD:
    case XED_ICLASS_UNPCKLPD:
    case XED_ICLASS_EXTRACTPS:
    case XED_ICLASS_INSERTPS:
    case XED_ICLASS_CVTPD2PS:
    case XED_ICLASS_CVTSD2SS:
    case XED_ICLASS_CVTPS2PD:
    case XED_ICLASS_CVTSS2SD:
    case XED_ICLASS_CVTDQ2PS:
    case XED_ICLASS_CVTPS2DQ:
    case XED_ICLASS_CVTTPS2DQ:
    case XED_ICLASS_CVTDQ2PD:
    case XED_ICLASS_CVTPD2DQ:
    case XED_ICLASS_CVTTPD2DQ:
    case XED_ICLASS_CVTPI2PS:
    case XED_ICLASS_CVTPS2PI:
    case XED_ICLASS_CVTTPS2PI:
    case XED_ICLASS_CVTPI2PD:
    case XED_ICLASS_CVTPD2PI:
    case XED_ICLASS_CVTTPD2PI:
    case XED_ICLASS_CVTSI2SS:
    case XED_ICLASS_CVTSS2SI:
    case XED_ICLASS_CVTTSS2SI:
    case XED_ICLASS_CVTSI2SD:
    case XED_ICLASS_CVTSD2SI:
    case XED_ICLASS_CVTTSD2SI:
    case XED_ICLASS_COMISS:
    case XED_ICLASS_COMISD:
    case XED_ICLASS_UCOMISS:
    case XED_ICLASS_UCOMISD:
    case XED_ICLASS_MAXSS:
    case XED_ICLASS_MAXSD:
    case XED_ICLASS_MAXPS:
    case XED_ICLASS_MAXPD:
    case XED_ICLASS_MINSS:
    case XED_ICLASS_MINSD:
    case XED_ICLASS_MINPS:
    case XED_ICLASS_MINPD:
    case XED_ICLASS_ROUNDPS:
    case XED_ICLASS_ROUNDPD:
    case XED_ICLASS_DPPS:
    case XED_ICLASS_DPPD:
    case XED_ICLASS_ANDPS:
    case XED_ICLASS_ANDPD:
    case XED_ICLASS_ANDNPS:
    case XED_ICLASS_ANDNPD:
    case XED_ICLASS_ORPS:
    case XED_ICLASS_ORPD:
    case XED_ICLASS_XORPS:
    case XED_ICLASS_XORPD:
      is_vls = true;
      break;
    default:
      break;
  }

  return is_vls;
}

Decoder::decoder_reg X86Decoder::last_reg()
{
    return XED_REG_LAST;
}


// TODO move part of this to superclass? possible in c++?
X86DecodedInst::X86DecodedInst(Decoder* d, const uint8_t * code, size_t size, uint64_t address)
{
  this->m_dec = d;
  this->m_code = code;
  this->m_size = size;
  this->m_address = address;
  this->m_already_decoded = false;
  // this->xed_inst is not initialized, do not try to get its disassembly
  //this->set_disassembly();
}

xed_decoded_inst_t * X86DecodedInst::get_xed_inst()
{
  return &xed_inst;
}

unsigned int X86DecodedInst::inst_num_id() const
{
  return xed_decoded_inst_get_iclass(static_cast<const xed_decoded_inst_t*>(&(this->xed_inst)));
}

void X86DecodedInst::set_disassembly()
{
  xed_syntax_enum_t s;
  char temp_buffer[64];
  
  // Choose the XED syntax
  switch(m_dec->get_syntax()){
    case DL_SYNTAX_ATT:
      s = XED_SYNTAX_ATT;
      break;
    case DL_SYNTAX_XED:
      s = XED_SYNTAX_XED;
      break;
    case DL_SYNTAX_INTEL: 
    case DL_SYNTAX_DEFAULT:
    default:
      s = XED_SYNTAX_INTEL;
  }
  
  // Disassemble the decoded instruction to out_buffer using the specified syntax
  xed_format_context(s, (&(this->xed_inst)), temp_buffer, 
                      sizeof(temp_buffer) - 1, this->m_address, 0, 0);
  
  m_disassembly.assign(temp_buffer, sizeof(temp_buffer));
}

std::string X86DecodedInst::disassembly_to_str() const
{
  return this->m_disassembly;
}

bool X86DecodedInst::is_nop() const
{
  return xed_decoded_inst_get_attribute(static_cast<const xed_decoded_inst_t*>(&(this->xed_inst)),
                                        XED_ATTRIBUTE_NOP);
}

bool X86DecodedInst::is_atomic() const
{
  const xed_operand_values_t* ops = 
      xed_decoded_inst_operands_const(static_cast<const xed_decoded_inst_t*>(&(this->xed_inst)));
  return xed_operand_values_get_atomic(ops);
}

bool X86DecodedInst::is_prefetch() const
{
  return xed_decoded_inst_is_prefetch(static_cast<const xed_decoded_inst_t*>(&(this->xed_inst)));
}

bool X86DecodedInst::is_serializing() const
{
  bool is_serializing = false;
  switch(xed_decoded_inst_get_iclass(static_cast<const xed_decoded_inst_t*>(&(this->xed_inst)))) {
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
      is_serializing = true;
      break;
    default:
      is_serializing = false;
      break;
  }
  return is_serializing;
}

bool X86DecodedInst::is_conditional_branch() const
{
  return xed_decoded_inst_get_category(
      static_cast<const xed_decoded_inst_t*>(&(this->xed_inst))) == XED_CATEGORY_COND_BR;
}


bool X86DecodedInst::is_indirect_branch() const
{
  bool is_b = false;
  
  switch(xed_decoded_inst_get_iclass(static_cast<const xed_decoded_inst_t*>(&(this->xed_inst))))
  {
    case XED_ICLASS_JMP:
    case XED_ICLASS_JMP_FAR:
      is_b = true;
      break;
    default:
      break;
  }
  
  return is_b;
}

bool X86DecodedInst::is_barrier() const
{
  return (xed_decoded_inst_get_iclass(static_cast<const xed_decoded_inst_t*>(&(this->xed_inst))) == XED_ICLASS_MFENCE
          || xed_decoded_inst_get_iclass(static_cast<const xed_decoded_inst_t*>(&(this->xed_inst))) == XED_ICLASS_LFENCE
          || xed_decoded_inst_get_iclass(static_cast<const xed_decoded_inst_t*>(&(this->xed_inst))) == XED_ICLASS_SFENCE);
}

bool X86DecodedInst::src_dst_merge() const
{
  return ((xed_decoded_inst_get_iclass(static_cast<const xed_decoded_inst_t*>(&(this->xed_inst))) == XED_ICLASS_MOVHPD)
            || (xed_decoded_inst_get_iclass(static_cast<const xed_decoded_inst_t*>(&(this->xed_inst))) == XED_ICLASS_MOVHPS)
            || (xed_decoded_inst_get_iclass(static_cast<const xed_decoded_inst_t*>(&(this->xed_inst))) == XED_ICLASS_MOVLPD)
            || (xed_decoded_inst_get_iclass(static_cast<const xed_decoded_inst_t*>(&(this->xed_inst))) == XED_ICLASS_MOVLPS)
            || (xed_decoded_inst_get_iclass(static_cast<const xed_decoded_inst_t*>(&(this->xed_inst))) == XED_ICLASS_MOVSD_XMM) // EXEC exists only for reg-to-reg moves
            || (xed_decoded_inst_get_iclass(static_cast<const xed_decoded_inst_t*>(&(this->xed_inst))) == XED_ICLASS_MOVSS));
}

bool X86DecodedInst::is_X87() const
{
  return (toupper(xed_iclass_enum_t2str(
            xed_decoded_inst_get_iclass(
              static_cast<const xed_decoded_inst_t*>(&(this->xed_inst))))[0]) == 'F');
}

bool X86DecodedInst::has_modifiers() const
{
  return false;
}

bool X86DecodedInst::is_mem_pair() const
{
  return false;
}

} // namespace dl;
