#ifndef _DECODER_H_
#define _DECODER_H_

#include <string>
#include <cassert>

namespace dl
{

// enums

/// Supported architectures
typedef enum dl_arch {
  DL_ARCH_INTEL,
  DL_ARCH_RISCV
} dl_arch;

/// Supported modes
typedef enum dl_mode {
  DL_MODE_32,
  DL_MODE_64
} dl_mode; 

/// Supported syntaxes
typedef enum dl_syntax {
  DL_SYNTAX_DEFAULT,
  DL_SYNTAX_INTEL,
  DL_SYNTAX_ATT,
  DL_SYNTAX_XED
} dl_syntax;

/// Supported ISAs / processor modes
typedef enum dl_isa {
  DL_ISA_IA32,
  DL_ISA_X86_64,
  DL_ISA_RISCV
} dl_isa;
  
class DecodedInst;
  
class Decoder
{
  public:
    typedef unsigned int decoder_reg;
    typedef unsigned int decoder_operand;
    typedef unsigned int decoder_opcode;
    
    static const decoder_operand DL_OPCODE_INVALID = 0;
    static const decoder_reg DL_REG_INVALID = 0;

    virtual ~Decoder();  // dtor
    virtual void decode(DecodedInst * inst) = 0;  // pure virtual method; implement in subclass
    virtual void decode(DecodedInst * inst, dl_isa isa) = 0;
    
    /// Change the ISA mode to new_mode
    virtual void change_isa_mode(dl_isa new_isa) = 0;
    
    /// Get the instruction name from the numerical (enum) instruction Id
    virtual const char* inst_name(unsigned int inst_id) = 0;
    
    /// Get the register name from the numerical (enum) register Id
    virtual const char* reg_name(unsigned int reg_id) = 0;
    
    /// Get the largest enclosing register; applies to x86 only; ARM just returns r
    virtual decoder_reg largest_enclosing_register(decoder_reg r) = 0;
    
    /// Check if this register is invalid
    virtual bool invalid_register(decoder_reg r) = 0;

    /// Check if this register holds the program counter
    virtual bool reg_is_program_counter(decoder_reg r) = 0;

    /// True if instruction belongs to instruction group/category
    virtual bool inst_in_group(const DecodedInst * inst, unsigned int group_id) = 0;
    
    /// Get the number of operands of any type for the specified instruction
    virtual unsigned int num_operands(const DecodedInst * inst) = 0;
    
    /// Get the number of memory operands of the specified instruction
    virtual unsigned int num_memory_operands(const DecodedInst * inst) = 0;
    
    /// Get the base register of the memory operand pointed by mem_idx
    virtual decoder_reg mem_base_reg (const DecodedInst * inst, unsigned int mem_idx) = 0;

    /// Get the index register of the memory operand pointed by mem_idx
    virtual decoder_reg mem_index_reg (const DecodedInst * inst, unsigned int mem_idx) = 0;

    /// Check if the operand mem_idx from instruction inst is read from memory
    virtual bool op_read_mem (const DecodedInst * inst, unsigned int mem_idx) = 0;

    /// Check if the operand mem_idx from instruction inst is written to memory
    virtual bool op_write_mem (const DecodedInst * inst, unsigned int mem_idx) = 0;
    
    /// Check if the operand idx from instruction inst reads from a register
    virtual bool op_read_reg (const DecodedInst * inst, unsigned int idx) = 0;

    /// Check if the operand idx from instruction inst writes a register
    virtual bool op_write_reg (const DecodedInst * inst, unsigned int idx) = 0;

    /// Check if the operand idx from instruction inst is involved in an address generation operation
    /// (i.e. part of LEA instruction in x86)
    virtual bool is_addr_gen (const DecodedInst * inst, unsigned int idx) = 0;

    /// Check if the operand idx from instruction inst is a register
    virtual bool op_is_reg (const DecodedInst * inst, unsigned int idx) = 0;

    /// Get the register used for operand idx from instruction inst.
    /// Function op_is_reg() should be called first.
    virtual decoder_reg get_op_reg (const DecodedInst * inst, unsigned int idx) = 0;
    
    /// Get the size in bytes of the memory operand pointed by mem_idx
    virtual unsigned int size_mem_op (const DecodedInst * inst, unsigned int mem_idx) = 0;
    
    /// Get the number of execution micro operations contained in instruction 'ins' 
    virtual unsigned int get_exec_microops(const DecodedInst *ins, int numLoads, int numStores) = 0;
    
    /// Get the maximum size of the operands of instruction inst in bits
    virtual uint16_t get_operand_size(const DecodedInst *ins) = 0;
    
    /// Check if the opcode is an instruction that performs a cache flush
    virtual bool is_cache_flush_opcode(decoder_opcode opcd) = 0;
    
    /// Check if the opcode is a division instruction
    virtual bool is_div_opcode(decoder_opcode opcd) = 0;
    
    /// Check if the opcode is a pause instruction
    virtual bool is_pause_opcode(decoder_opcode opcd) = 0;

    /// Check if the opcode is a branch instruction
    virtual bool is_branch_opcode(decoder_opcode opcd) = 0;
    
    /// Check if the opcode is an add/sub instruction that operates in vector and FP registers
    virtual bool is_fpvector_addsub_opcode(decoder_opcode opcd, const DecodedInst* ins) = 0;
    
    /// Check if the opcode is a mul/div instruction that operates in vector and FP registers
    virtual bool is_fpvector_muldiv_opcode(decoder_opcode opcd, const DecodedInst* ins) = 0;

    /// Check if the opcode is an instruction that loads or store data on vector and FP registers
    virtual bool is_fpvector_ldst_opcode(decoder_opcode opcd, const DecodedInst* ins) = 0;
  
    /// Get the value of the last register in the enumeration
    virtual decoder_reg last_reg() = 0;
    
    /// Get the target architecture of the decoder
    dl_arch get_arch();

    /// Get the target mode (32 or 64 bit) of the decoder
    dl_mode get_mode();

    /// Get the target syntax of the decoder
    dl_syntax get_syntax();
    
  protected:
    dl_arch m_arch;
    dl_mode m_mode;
    dl_syntax m_syntax;
    dl_isa m_isa;
    
};

class DecodedInst
{
  public:
    virtual ~DecodedInst();  // dtor -- virtual to be able to destroy polymorphically
    const bool & get_already_decoded() const;
    void set_already_decoded(const bool & b);
    size_t & get_size();
    const uint8_t * & get_code();
    uint64_t & get_address();
        
    /// Get the instruction numerical Id
    virtual unsigned int inst_num_id() const = 0;
    
    /// Get an string with the disassembled instruction
    virtual void disassembly_to_str(char *, int) const = 0;
    
    /// Check if this instruction is a NOP
    virtual bool is_nop() const = 0;
    
    /// Check if this instruction is atomic
    virtual bool is_atomic() const = 0;
    
    /// Check if instruction is a prefetch
    virtual bool is_prefetch() const = 0;
    
    /// Check if this instruction is serializing (all previous instructions must have been executed)
    virtual bool is_serializing() const = 0;

    /// Check if this instruction is a conditional branch
    virtual bool is_conditional_branch() const = 0;

    /// Check if this instruction is a fence/barrier-type
    virtual bool is_barrier() const = 0;

    /// Check if in this instruction the result merges the source and destination.
    /// For instance: memory to XMM loads in x86.
    virtual bool src_dst_merge() const = 0;
    
    /// Check if this instruction is from he X87 set (only applicable to Intel instructions)
    virtual bool is_X87() const = 0;
        
    /// Check if this instruction has shift or extender modifiers (ARM)
    virtual bool has_modifiers() const = 0;
    
    /// Check if this instruction loads or stores pairs of registers using a memory address (ARM)
    virtual bool is_mem_pair() const = 0;
    
  protected:
    /// True if the decoding phase has already happened
    bool m_already_decoded;
    
    /// Pointer to the Decoder in use
    Decoder* m_dec;
    
    /// Instruction's size
    size_t m_size;
    
    /// Instruction's code pre-disassembly
    const uint8_t * m_code;
    
    /// Instruction's address
    uint64_t m_address;
};

class DecoderFactory
{
  public:
    /// Creates a Decoder object with the specified target architecture, mode and syntax
    Decoder *CreateDecoder(dl_arch arch, dl_mode mode, dl_syntax syntax);
    
    /// Creates an instruction that follows the syntax and targets the architecures of the Decoder d
    DecodedInst *CreateInstruction(Decoder * d, const uint8_t * code, size_t size, uint64_t addr);
};

} // namespace dl;

#endif // _DECODER_H_
