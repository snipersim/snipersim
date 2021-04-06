#ifndef _X86_DECODER_H_
#define _X86_DECODER_H_

#include "decoder.h"

extern "C" {
  #include <xed-reg-enum.h>
  #include <xed-interface.h>
}

namespace dl
{
  
class X86Decoder : public Decoder
{
  public:    
    X86Decoder(dl_arch arch, dl_mode mode, dl_syntax syntax);
    virtual ~X86Decoder();
    virtual void decode(DecodedInst * inst) override;
    virtual void decode(DecodedInst * inst, dl_isa isa) override;
    virtual void change_isa_mode(dl_isa new_isa) override;
    virtual const char* inst_name(unsigned int inst_id) override;
    virtual const char* reg_name(unsigned int reg_id) override;
    virtual decoder_reg largest_enclosing_register(decoder_reg r) override;
    virtual bool invalid_register(decoder_reg r) override;
    virtual bool reg_is_program_counter(decoder_reg r) override;
    virtual bool inst_in_group (const DecodedInst * inst, unsigned int group_id) override;
    virtual unsigned int num_operands (const DecodedInst * inst) override;
    virtual unsigned int num_memory_operands (const DecodedInst * inst) override;
    virtual decoder_reg mem_base_reg (const DecodedInst * inst, unsigned int mem_idx) override;
    virtual bool mem_base_upate(const DecodedInst *inst, unsigned int mem_idx) override { return false; }
    virtual bool has_index_reg (const DecodedInst * inst, unsigned int mem_idx) override { return true; }
    virtual decoder_reg mem_index_reg (const DecodedInst * inst, unsigned int mem_idx) override;
    virtual bool op_read_mem (const DecodedInst * inst, unsigned int mem_idx) override;
    virtual bool op_write_mem (const DecodedInst * inst, unsigned int mem_idx) override;
    virtual bool op_read_reg (const DecodedInst * inst, unsigned int idx) override;
    virtual bool op_write_reg (const DecodedInst * inst, unsigned int idx) override;
    virtual bool is_addr_gen (const DecodedInst * inst, unsigned int idx) override;
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
    virtual uint32_t map_register(decoder_reg reg) override { return reg; }
    virtual unsigned int num_read_implicit_registers(const DecodedInst *inst) override {return 0;}
    virtual decoder_reg get_read_implicit_reg(const DecodedInst* inst, unsigned int idx) override { return 0; }
    virtual unsigned int num_write_implicit_registers(const DecodedInst *inst) override {return 0; }
    virtual decoder_reg get_write_implicit_reg(const DecodedInst *inst, unsigned int idx) override { return 0; }

  private:
    // Methods
    const xed_operand_t * get_operand(const DecodedInst * inst, unsigned int idx);
    xed_operand_enum_t get_operand_name(const DecodedInst * inst, unsigned int idx);
    // Variables
    xed_state_t m_xed_state_init;

};

class X86DecodedInst : public DecodedInst
{
  public:
    X86DecodedInst(Decoder* d, const uint8_t * code, size_t size, uint64_t address);
    xed_decoded_inst_t * get_xed_inst();
    
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
    virtual bool is_writeback() const override { return false; }

  private:
    void set_disassembly();
    xed_decoded_inst_t xed_inst;

};

} // namespace dl;

#endif // _X86_DECODER_H_
