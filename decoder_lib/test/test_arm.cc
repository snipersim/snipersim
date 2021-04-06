#include <decoder.h>
#include <arm_decoder.h>  // For testing, normally this wouldn't be included
#include <capstone.h>
#include <iostream>
#include <type_traits>

#define ARM_CODE "\xED\xFF\xFF\xEB\x04\xe0\x2d\xe5\x00\x00\x00\x00\xe0\x83\x22\xe5\xf1\x02\x03\x0e\x00\x00\xa0\xe3\x02\x30\xc1\xe7\x00\x00\x53\xe3"
#define ARM_CODE2 "\x10\xf1\x10\xe7\x11\xf2\x31\xe7\xdc\xa1\x2e\xf3\xe8\x4e\x62\xf3"
#define ARM_CODE3 "\x02\x30\xc1\xe7"
#define ARM_CODE4 "\xff\xf7\xce\xe9"  // testing
#define ARM_CODE5 "\x45\xfe\x02\xf0"  // testing
#define ARM_CODE6 "\x90\x47"  // testing
#define ARM_CODE_PREINDEX "\x41\x4c\x40\xf8"  // ldr X1, [X2, #4]!
#define ARM_CODE_POSTINDEX "\x41\x44\x40\xF8"  // ldr X1, [X2], #4
#define ARM_CODE_IMMOFF "\x22\x04\x40\xB9"  // ldr W2, [X1, #4]
#define ARM_LDP1 "\x02\x04\x40\xA9"  //  LDP X2, X1, [X0]
#define ARM_SCVTF "\x22\xFC\x42\x9E"  // SCVTF  D2, X1, 1
#define ARM_FCVTAS "\x00\x00\x24\x9E"  // FCVTAS  X0, S0
#define ARM_DUP "\x00\x0C\x01\x0E"  // dup v0.8b,w0
#define ARM_SMOV "\x2B\x2E\x04\x4E"  // smov x11, v17.s[0]
#define ARM_EOR "\x41\x04\x41\x4A"  // eor w1, w2, w1, lsr #1

int main(int argc, const char* argv[])
{
  dl::DecoderFactory *f = new dl::DecoderFactory;
  dl::Decoder *d = f->CreateDecoder(dl::DL_ARCH_ARMv8, dl::DL_MODE_64, dl::DL_SYNTAX_DEFAULT);
  
  dl::Decoder::decoder_reg r1 = ARM_REG_CPSR;
  dl::Decoder::decoder_reg r2 = ARM_REG_R5;
  
  std::cout << "ARM register: " << r1 << "; name: " << d->reg_name(r1) << std::endl;
  std::cout << "ARM instruction: " << ARM_INS_VSQRT << "; name: " << d->inst_name(ARM_INS_VSQRT) \
    << std::endl;
  std::cout << "ARM extended register test: " << r2 << " is " << d->largest_enclosing_register(r2) \
    << std::endl;

  // Decode test
  
  //d->change_isa_mode(dl::DL_ISA_ARM32);
  uint8_t * code = (unsigned char*)ARM_CODE;
  size_t size = sizeof(ARM_CODE) - 1;
  uint64_t addr = 0x1000;  // some generic address
  // create instruction to be decoded
  dl::DecodedInst *i = f->CreateInstruction(d, code, size, addr);
  // decode instruction
  d->decode(i);
  // see contents
  cs_insn * insn = ((dl::ARMDecodedInst *)i)->get_capstone_inst();
  printf("\n0x%" PRIx64 ":\t%s\t\t%s // insn-ID: %u, insn-mnem: %s\n",
					insn->address, insn->mnemonic, insn->op_str,
					insn->id, d->inst_name(insn->id));

  // Extra functions after decoding
  
  // Check group of instruction
  std::cout << "Group of instruction is : ";
  for ( int group = ARM_GRP_INVALID; group != ARM_GRP_ENDING; group++ )
  {
    arm_insn_group g = static_cast<arm_insn_group>(group);
    if (d->inst_in_group(i, g))
      std::cout << g << " ";
  }
  std::cout << std::endl;
    
  // Check Id of instruction
  std::cout << "Instruction Id: " << i->inst_num_id() << std::endl;
  std::cout << "Is instruction a NOP? " << i->is_nop() << std::endl;
  std::cout << "Is instruction atomic? " << i->is_atomic() << std::endl;
  // Prefetch 
  std::cout << "Is prefetch? " << i->is_prefetch() << std::endl;
  unsigned int n_ops = d->num_operands(i);
  std::cout << "Number of operands: " << d->num_operands(i) << std::endl;
  unsigned int nmemops = d->num_memory_operands(i);
  std::cout << "Number of memory operands: " << nmemops << std::endl;

  cs_detail *detail = insn->detail;

  std::cout << "Number of destiny implicit registers: " << detail->regs_read_count << std::endl;
  std::cout << "NUmber of source implicit registers: " << detail->regs_write_count << std::endl;

  for(uint32_t mem_idx = 0; mem_idx < nmemops; ++mem_idx)
  {
    std::cout << " Operand " << mem_idx << " base reg: " << d->mem_base_reg(i, mem_idx) << std::endl;
    std::cout << " Operand " << mem_idx << " index reg: " << d->mem_index_reg(i, mem_idx) << std::endl;
    std::cout << " Operand " << mem_idx << " size: " << d->size_mem_op(i, mem_idx) << std::endl;
    std::cout << " Operand " << mem_idx << " reads memory: " << d->op_read_mem(i, mem_idx) << std::endl;
    std::cout << " Operand " << mem_idx << " writes memory: " << d->op_write_mem(i, mem_idx) << std::endl;
/*    cs_insn * csi = ((dl::ARMDecodedInst *)i)->get_capstone_inst();
    int index = ((dl::ARMDecoder *)d)->index_mem_op(csi, mem_idx);
    cs_arm64 * arm64 = &(csi->detail->arm64);
    std::cout << " Operand " << mem_idx << " disp: " << arm64->operands[index].mem.disp << std::endl;
    std::cout << " Operand " << mem_idx << " shift: " << arm64->operands[index].shift.type << std::endl;
    std::cout << " Operand " << mem_idx << " shift value: " << arm64->operands[index].shift.value << std::endl;    
    std::cout << " Operand " << mem_idx << " extender: " << arm64->operands[index].ext << std::endl;
    std::cout << " Instruction writebacks: " << arm64->writeback << std::endl;*/
  }
  for(uint32_t idx = 0; idx < n_ops; ++idx)
  {
    cs_insn * csi = ((dl::ARMDecodedInst *)i)->get_capstone_inst();
    cs_arm64 * arm64 = &(csi->detail->arm64);
    std::cout << " Operand " << idx << " shift: " << arm64->operands[idx].shift.type << std::endl;
    std::cout << " Operand " << idx << " shift value: " << arm64->operands[idx].shift.value << std::endl;   
  }

  // Disassembly
  std::string dis_str = i->disassembly_to_str();

  std::cout << "Disassembly: " << std::endl;
  std::cout << dis_str << std::endl;
}
