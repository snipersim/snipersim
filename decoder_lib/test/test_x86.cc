#include <decoder.h>
#include <x86_decoder.h>  // For testing, normally this wouldn't be included
#include <iostream>

extern "C" 
{
#include <xed-reg-enum.h>
#include <xed-iclass-enum.h>
} 

#define X86_CODE64 "\x55\x48\x8b\x05\xb8\x13\x00\x00"

int main(int argc, const char* argv[])
{
  // Decoder engine creation
  
  dl::DecoderFactory *f = new dl::DecoderFactory;
  dl::Decoder *d = f->CreateDecoder(dl::DL_ARCH_INTEL, dl::DL_MODE_64, dl::DL_SYNTAX_INTEL);
  
  // Some registers for the test
  
  dl::Decoder::decoder_reg r1 = XED_REG_CX;
  dl::Decoder::decoder_reg r2 = XED_REG_AX;
  
  // Basic functionality tests
  
  std::cout << "X86 register: " << r1 << "; name: " << d->reg_name(r1) <<std::endl;
  std::cout << "X86 instruction: " << XED_ICLASS_VFMADDSUBPD << "; name: " \
    << d->inst_name(XED_ICLASS_VFMADDSUBPD) << std::endl;
  std::cout << "X86 extended register test: " << r2 << " is " << d->largest_enclosing_register(r2) \
    << std::endl;

  // Decode test
  
  uint8_t * code = (unsigned char*)X86_CODE64;
  size_t size = sizeof(X86_CODE64) - 1;
  uint64_t addr = 0x1000;  // some generic address
  // create instruction to be decoded
  dl::DecodedInst *i = f->CreateInstruction(d, code, size, addr);
  // decode instruction
  d->decode(i);
  // see contents
  char testbuf[256];
  xed_decoded_inst_dump ((((dl::X86DecodedInst *)i)->get_xed_inst()), testbuf, 256);  
  std::cout << "Decoded instruction:" << std::endl;
  std::cout << testbuf << std::endl;
  
  // Extra functions after decoding
  
  // Check category of instruction
  std::cout << "Category of instruction is : ";
  for ( int category = XED_CATEGORY_INVALID; category != XED_CATEGORY_LAST; category++ )
  {
    xed_category_enum_t c = static_cast<xed_category_enum_t>(category);
    if (d->inst_in_group(i, c))
      std::cout << c << " ";
  }
  std::cout << std::endl;

  // Check Id of instruction
  std::cout << "Instruction Id: " << i->inst_num_id() << std::endl;
  std::cout << "Is instruction a NOP? " << i->is_nop() << std::endl;
  std::cout << "Is instruction atomic? " << i->is_atomic() << std::endl;
  // Prefetch 
  std::cout << "Is prefetch? " << i->is_prefetch() << std::endl;
  std::cout << "Number of operands : " << d->num_operands(i) << std::endl;
  unsigned int nmemops = d->num_memory_operands(i);
  std::cout << "Number of memory operands: " << nmemops << std::endl;
  for(uint32_t mem_idx = 0; mem_idx < nmemops; ++mem_idx)
  {
    std::cout << " Operand " << mem_idx << " base reg: " << d->mem_base_reg(i, mem_idx) << std::endl;
    std::cout << " Operand " << mem_idx << " index reg: " << d->mem_index_reg(i, mem_idx) << std::endl;
    std::cout << " Operand " << mem_idx << " size: " << d->size_mem_op(i, mem_idx) << std::endl;
    std::cout << " Operand " << mem_idx << " reads memory: " << d->op_read_mem(i, mem_idx) << std::endl;
    std::cout << " Operand " << mem_idx << " writes memory: " << d->op_write_mem(i, mem_idx) << std::endl;
  }
  
  // Disassembly
  //char disassembly[64];
  //xed_format_context(XED_SYNTAX_INTEL, ((dl::X86DecodedInst *)i)->get_xed_inst(), disassembly,
  //                   sizeof(disassembly) - 1, addr, 0, 0);
  std::string dis_str = i->disassembly_to_str();

  std::cout << "Disassembly: " << std::endl;
  std::cout << dis_str << std::endl;

}