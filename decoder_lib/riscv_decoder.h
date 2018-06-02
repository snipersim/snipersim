#ifndef _RISCV_DECODER_H_
#define _RISCV_DECODER_H_

#include "decoder.h"

#include <cstddef>
#include <asm/types.h>
#include <asm/meta.h>
#include <asm/codec.h>
#include <asm/switch.h>
#include <asm/strings.h>
#include <asm/host-endian.h>
#include <util/fmt.h>
#include <util/util.h>

namespace dl
{

  enum reg_num
  {
    rv_ireg_x0,                         /* Hard-wired zero */
    rv_ireg_x1,                         /* Return address Caller */
    rv_ireg_x2,                         /* Stack pointer Callee */
    rv_ireg_x3,                         /* Global pointer */
    rv_ireg_x4,                         /* Thread pointer Callee */
    rv_ireg_x5,                         /* Temporaries Caller */
    rv_ireg_x6,                         /* Temporaries Caller */
    rv_ireg_x7,                         /* Temporaries Caller */
    rv_ireg_x8,                         /* Saved register/frame pointer Callee */
    rv_ireg_x9,                         /* Saved registers Callee */
    rv_ireg_x10,                        /* Function arguments Caller */
    rv_ireg_x11,                        /* Function arguments Caller */
    rv_ireg_x12,                        /* Function arguments Caller */
    rv_ireg_x13,                        /* Function arguments Caller */
    rv_ireg_x14,                        /* Function arguments Caller */
    rv_ireg_x15,                        /* Function arguments Caller */
    rv_ireg_x16,                        /* Function arguments Caller */
    rv_ireg_x17,                        /* Function arguments Caller */
    rv_ireg_x18,                        /* Saved registers Callee */
    rv_ireg_x19,                        /* Saved registers Callee */
    rv_ireg_x20,                        /* Saved registers Callee */
    rv_ireg_x21,                        /* Saved registers Callee */
    rv_ireg_x22,                        /* Saved registers Callee */
    rv_ireg_x23,                        /* Saved registers Callee */
    rv_ireg_x24,                        /* Saved registers Callee */
    rv_ireg_x25,                        /* Saved registers Callee */
    rv_ireg_x26,                        /* Saved registers Callee */
    rv_ireg_x27,                        /* Saved registers Callee */
    rv_ireg_x28,                        /* Temporaries Caller */
    rv_ireg_x29,                        /* Temporaries Caller */
    rv_ireg_x30,                        /* Temporaries Caller */
    rv_ireg_x31,                        /* Temporaries Caller */
    rv_freg_f0,                         /* FP temporaries Caller */
    rv_freg_f1,                         /* FP temporaries Caller */
    rv_freg_f2,                         /* FP temporaries Caller */
    rv_freg_f3,                         /* FP temporaries Caller */
    rv_freg_f4,                         /* FP temporaries Caller */
    rv_freg_f5,                         /* FP temporaries Caller */
    rv_freg_f6,                         /* FP temporaries Caller */
    rv_freg_f7,                         /* FP temporaries Caller */
    rv_freg_f8,                         /* FP saved registers Callee */
    rv_freg_f9,                         /* FP saved registers Callee */
    rv_freg_f10,                        /* FP arguments Caller */
    rv_freg_f11,                        /* FP arguments Caller */
    rv_freg_f12,                        /* FP arguments Caller */
    rv_freg_f13,                        /* FP arguments Caller */
    rv_freg_f14,                        /* FP arguments Caller */
    rv_freg_f15,                        /* FP arguments Caller */
    rv_freg_f16,                        /* FP arguments Caller */
    rv_freg_f17,                        /* FP arguments Caller */
    rv_freg_f18,                        /* FP saved registers Callee */
    rv_freg_f19,                        /* FP saved registers Callee */
    rv_freg_f20,                        /* FP saved registers Callee */
    rv_freg_f21,                        /* FP saved registers Callee */
    rv_freg_f22,                        /* FP saved registers Callee */
    rv_freg_f23,                        /* FP saved registers Callee */
    rv_freg_f24,                        /* FP saved registers Callee */
    rv_freg_f25,                        /* FP saved registers Callee */
    rv_freg_f26,                        /* FP saved registers Callee */
    rv_freg_f27,                        /* FP saved registers Callee */
    rv_freg_f28,                        /* FP temporaries Caller */
    rv_freg_f29,                        /* FP temporaries Caller */
    rv_freg_f30,                        /* FP temporaries Caller */
    rv_freg_f31,                        /* FP temporaries Caller */
    last_reg
  };
extern const char* reg_name_sym[];  
  
class RISCVDecoder : public Decoder
{
  public:    
    RISCVDecoder(dl_arch arch, dl_mode mode, dl_syntax syntax);
    int reg_set_size = 64;   
    
    virtual ~RISCVDecoder();  // dtor
    
    virtual void decode(DecodedInst * inst) override; // pure virtual method; implement in subclass
    virtual void decode(DecodedInst * inst, dl_isa isa) override;
    
    /// Change the ISA mode to new_mode
    virtual void change_isa_mode(dl_isa new_isa) override; 
    
    /// Get the instruction name from the numerical (enum) instruction Id
    virtual const char* inst_name(unsigned int inst_id) override; 
    /// Get the register name from the numerical (enum) register Id
    virtual const char* reg_name(unsigned int reg_id) override;
    
     /// Get the largest enclosing register; applies to x86 only; ARM just returns r; RISCV ?
    virtual decoder_reg largest_enclosing_register(decoder_reg r) override;
    /// Check if this register is invalid
    virtual bool invalid_register(decoder_reg r) override;
   
    /// Check if this register holds the program counter
    virtual bool reg_is_program_counter(decoder_reg r) override;

    /// True if instruction belongs to instruction group/category
    virtual bool inst_in_group (const DecodedInst * inst, unsigned int group_id) override;
    /// Get the number of operands of any type for the specified instruction
    virtual unsigned int num_operands (const DecodedInst * inst) override;
    /// Get the number of memory operands of the specified instruction
    virtual unsigned int num_memory_operands (const DecodedInst * inst) override;

    /// Get the base register of the memory operand pointed by mem_idx
    virtual decoder_reg mem_base_reg (const DecodedInst * inst, unsigned int mem_idx) override;
    /// Get the index register of the memory operand pointed by mem_idx
    virtual decoder_reg mem_index_reg (const DecodedInst * inst, unsigned int mem_idx) override;
    
    /// Check if the operand mem_idx from instruction inst is read from memory
    virtual bool op_read_mem (const DecodedInst * inst, unsigned int mem_idx) override;
    /// Check if the operand mem_idx from instruction inst is written to memory
    virtual bool op_write_mem (const DecodedInst * inst, unsigned int mem_idx) override;
    /// Check if the operand idx from instruction inst reads from a register
    virtual bool op_read_reg (const DecodedInst * inst, unsigned int idx) override;
    /// Check if the operand idx from instruction inst writes a register
    virtual bool op_write_reg (const DecodedInst * inst, unsigned int idx) override;
    
    /// Check if the operand idx from instruction inst is involved in an address generation operation
    /// (i.e. part of LEA instruction in x86)
    virtual bool is_addr_gen (const DecodedInst * inst, unsigned int idx) override;
    /// Check if the operand idx from instruction inst is a register
    virtual bool op_is_reg (const DecodedInst * inst, unsigned int idx) override;    
    
    /// Get the register used for operand idx from instruction inst.
    /// Function op_is_reg() should be called first.
    virtual decoder_reg get_op_reg (const DecodedInst * inst, unsigned int idx) override;
    
    /// Get the size in bytes of the memory operand pointed by mem_idx
    virtual unsigned int size_mem_op (const DecodedInst * inst, unsigned int mem_idx) override;
    /// Get the number of execution micro operations contained in instruction 'ins' 
    virtual unsigned int get_exec_microops(const DecodedInst *ins, int numLoads, int numStores) override;
    /// Get the maximum size of the operands of instruction inst in bits
    virtual uint16_t get_operand_size(const DecodedInst *ins) override;
    /// Check if the opcode is an instruction that performs a cache flush
    virtual bool is_cache_flush_opcode(decoder_opcode opcd) override;

    /// Check if the opcode is a division instruction
    virtual bool is_div_opcode(decoder_opcode opcd) override;
    /// Check if the opcode is a pause instruction
    virtual bool is_pause_opcode(decoder_opcode opcd) override;
    /// Check if the opcode is a branch instruction
    virtual bool is_branch_opcode(decoder_opcode opcd) override;

    /// Check if the opcode is an add/sub instruction that operates in vector and FP registers
    virtual bool is_fpvector_addsub_opcode(decoder_opcode opcd, const DecodedInst* ins) override;
    /// Check if the opcode is a mul/div instruction that operates in vector and FP registers
    virtual bool is_fpvector_muldiv_opcode(decoder_opcode opcd, const DecodedInst* ins) override;    
    /// Check if the opcode is an instruction that loads or store data on vector and FP registers
    virtual bool is_fpvector_ldst_opcode(decoder_opcode opcd, const DecodedInst* ins) override;
    
    /// Get the value of the last register in the enumeration
    virtual decoder_reg last_reg() override;

    // /// Get the target architecture of the decoder
    // dl_arch get_arch();

    // /// Get the target mode (32 or 64 bit) of the decoder
    // dl_mode get_mode();

    // /// Get the target syntax of the decoder
    // dl_syntax get_syntax();

};

class RISCVDecodedInst : public DecodedInst
{
  public:
    RISCVDecodedInst(Decoder* d, const uint8_t * code, size_t size, uint64_t address);
    riscv::inst_t * get_rv8_inst();
    riscv::decode * get_rv8_dec();
    void set_rv8_dec(riscv::decode d);

    /// Get the instruction numerical Id
    virtual unsigned int inst_num_id() const override;
    /// Get an string with the disassembled instruction
    virtual void disassembly_to_str(char *, int) const override;
    /// Check if this instruction is a NOP
    virtual bool is_nop() const override;
    /// Check if this instruction is atomic
    virtual bool is_atomic() const override;
    /// Check if instruction is a prefetch
    virtual bool is_prefetch() const override;
    /// Check if this instruction is serializing (all previous instructions must have been executed)
    virtual bool is_serializing() const override;
    /// Check if this instruction is a conditional branch
    virtual bool is_conditional_branch() const override;
    /// Check if this instruction is a fence/barrier-type
    virtual bool is_barrier() const override;
    /// Check if in this instruction the result merges the source and destination.
    /// For instance: memory to XMM loads in x86.
    virtual bool src_dst_merge() const override;
    /// Check if this instruction is from he X87 set (only applicable to Intel instructions)
    virtual bool is_X87() const override;
    /// Check if this instruction has shift or extender modifiers (ARM)
    virtual bool has_modifiers() const override;
    /// Check if this instruction loads or stores pairs of registers using a memory address (ARM)
    virtual bool is_mem_pair() const override;

    // virtual ~DecodedInst();  // dtor -- virtual to be able to destroy polymorphically
    
    // const bool & get_already_decoded() const;
    // void set_already_decoded(const bool & b);
    // size_t & get_size();
    // const uint8_t * & get_code();
    // uint64_t & get_address();

    private:
     riscv::decode rv8_dec;
     riscv::inst_t rv8_instr;  
};

} // namespace dl;

#endif // _RISCV_DECODER_H_
