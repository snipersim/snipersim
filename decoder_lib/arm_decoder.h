#ifndef _ARM_DECODER_H_
#define _ARM_DECODER_H_

#include "decoder.h"

#include <capstone.h>

namespace dl
{
  
class ARMDecoder : public Decoder
{
  public:    
    // Methods
    ARMDecoder(dl_arch arch, dl_mode mode, dl_syntax syntax);
    virtual ~ARMDecoder();
    virtual void decode(DecodedInst * inst) override;
    virtual void decode(DecodedInst * inst, dl_isa isa) override;
    virtual void change_isa_mode(dl_isa new_isa) override;
    virtual const char* inst_name(unsigned int inst_id) override;
    virtual const char* reg_name(unsigned int reg_id) override;
    virtual decoder_reg largest_enclosing_register(decoder_reg r) override;
    virtual bool invalid_register(decoder_reg r) override;
    virtual bool reg_is_program_counter(decoder_reg r) override;
    virtual bool inst_in_group(const DecodedInst * inst, unsigned int group_id) override;    
    virtual unsigned int num_operands(const DecodedInst * inst) override;
    virtual unsigned int num_memory_operands(const DecodedInst * inst) override;
    virtual decoder_reg mem_base_reg (const DecodedInst * inst, unsigned int mem_idx) override;
    virtual bool mem_base_upate(const DecodedInst * inst, unsigned int mem_idx) override;
    virtual bool has_index_reg (const DecodedInst * inst, unsigned int mem_idx) override;
    virtual decoder_reg mem_index_reg (const DecodedInst * inst, unsigned int mem_idx) override;
    virtual bool op_read_mem(const DecodedInst * inst, unsigned int mem_idx) override;
    virtual bool op_write_mem(const DecodedInst * inst, unsigned int mem_idx) override;
    virtual bool op_read_reg (const DecodedInst * inst, unsigned int idx) override;
    virtual bool op_write_reg (const DecodedInst * inst, unsigned int idx) override;
    virtual bool is_addr_gen(const DecodedInst * inst, unsigned int idx) override;    
    virtual bool op_is_reg (const DecodedInst * inst, unsigned int idx) override;    
    virtual decoder_reg get_op_reg (const DecodedInst * inst, unsigned int idx) override;
    virtual unsigned int size_mem_op (const DecodedInst * inst, unsigned int mem_idx) override;
    virtual unsigned int get_exec_microops(const DecodedInst *ins, int numLoads, int numStores) override;
    virtual uint16_t get_operand_size(const DecodedInst *ins) override;
    virtual bool is_cache_flush_opcode(decoder_opcode opcd) override;
    virtual bool is_div_opcode(decoder_opcode opcd) override;
    virtual bool is_pause_opcode(decoder_opcode opcd) override;
    virtual bool is_branch_opcode(decoder_opcode opcd) override;
    virtual bool is_fpvector_addsub_opcode(decoder_opcode opcd, const DecodedInst* ins) override;
    virtual bool is_fpvector_muldiv_opcode(decoder_opcode opcd, const DecodedInst* ins) override;  
    virtual bool is_fpvector_ldst_opcode(decoder_opcode opcd, const DecodedInst* ins) override;
    virtual decoder_reg last_reg() override;
    virtual uint32_t map_register(decoder_reg reg) override;
    virtual unsigned int num_read_implicit_registers(const DecodedInst *inst) override;
    virtual decoder_reg get_read_implicit_reg(const DecodedInst* inst, unsigned int idx) override;
    virtual unsigned int num_write_implicit_registers(const DecodedInst *inst) override;
    virtual decoder_reg get_write_implicit_reg(const DecodedInst *inst, unsigned int idx) override;
  
    const csh & get_handle() const;
    
  private:
    // Methods
    int index_mem_op(cs_insn * csi, unsigned int mem_idx);
    unsigned int size_mem_reg(const DecodedInst * inst);
    unsigned int get_bytes_vess(unsigned int vec_elem);
    unsigned int get_bytes_vas(unsigned int vec_elem);
    unsigned int get_simd_mem_bytes(const DecodedInst * inst);

    // Variables
    csh m_handle;
    csh m_handle_aux;  // For second ISA mode

};

class ARMDecodedInst : public DecodedInst
{
  public:
    ARMDecodedInst(Decoder* d, const uint8_t * code, size_t size, uint64_t address);
    ~ARMDecodedInst();
    cs_insn * get_capstone_inst();
    void set_disassembly();
    /// This instruction loads or stores pairs of values?
    
    virtual unsigned int inst_num_id() const override;
    virtual std::string disassembly_to_str() const override;
    virtual bool is_nop() const override;
    virtual bool is_atomic() const override;
    virtual bool is_prefetch() const override;
    virtual bool is_serializing() const override;
    virtual bool is_conditional_branch() const override;
    virtual bool is_indirect_branch() const override;
    virtual bool is_barrier() const override;
    virtual bool src_dst_merge() const override;
    virtual bool is_X87() const override;
    virtual bool has_modifiers() const override;
    virtual bool is_mem_pair() const override;
    virtual bool is_writeback() const override;

  private:
    //cs_regs regs_read, regs_write;
    //uint8_t read_count, write_count;
    cs_insn *capstone_inst;

};

} // namespace dl;

#endif // _ARM_DECODER_H_
