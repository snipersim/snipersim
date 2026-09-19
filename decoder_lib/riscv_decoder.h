/* Sniper's RISC-V decoder, driven entirely by the tables tools/riscv/gen_isa.py
 * generates from the RISC-V Unified Database (riscv_isa_gen.{h,cc}).  No rv8,
 * no Capstone: dropping a new specification in and regenerating is all it
 * takes to decode a new extension; the core model then costs it by the class
 * the policy assigns (see tools/riscv/isa_policy.yaml). */
#ifndef _RISCV_DECODER_H_
#define _RISCV_DECODER_H_

#include "decoder.h"
#include "riscv_isa.h"

namespace dl
{
  /* Register numbering shared with rvisa: x0-x31, f0-f31, v0-v31. */
  enum reg_num
  {
    rv_ireg_x0 = 0,
    rv_freg_f0 = 32,
    rv_vreg_v0 = 64,
    rv_mreg_m0 = 96,      /* AME matrix registers */
    rv_areg_a0 = 128,     /* AME accumulator registers */
    last_reg = 144
  };

  class RISCVDecodedInst;

  class RISCVDecoder : public Decoder
  {
  public:
    RISCVDecoder(dl_arch arch, dl_mode mode, dl_syntax syntax);
    virtual ~RISCVDecoder();

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
    virtual decoder_reg mem_base_reg(const DecodedInst * inst, unsigned int mem_idx) override;
    virtual bool mem_base_upate(const DecodedInst * inst, unsigned int mem_idx) override { return false; }
    virtual bool has_index_reg(const DecodedInst * inst, unsigned int mem_idx) override { return false; }
    virtual decoder_reg mem_index_reg(const DecodedInst * inst, unsigned int mem_idx) override;
    virtual bool op_read_mem(const DecodedInst * inst, unsigned int mem_idx) override;
    virtual bool op_write_mem(const DecodedInst * inst, unsigned int mem_idx) override;
    virtual bool op_read_reg(const DecodedInst * inst, unsigned int idx) override;
    virtual bool op_write_reg(const DecodedInst * inst, unsigned int idx) override;
    virtual bool is_addr_gen(const DecodedInst * inst, unsigned int idx) override { return false; }
    virtual bool op_is_reg(const DecodedInst * inst, unsigned int idx) override;
    virtual decoder_reg get_op_reg(const DecodedInst * inst, unsigned int idx) override;
    virtual unsigned int size_mem_op(const DecodedInst * inst, unsigned int mem_idx) override;
    virtual unsigned int get_exec_microops(const DecodedInst *ins, int numLoads, int numStores) override { return 1; }
    virtual uint32_t get_operand_size(const DecodedInst *ins) override;
    virtual bool is_cache_flush_opcode(decoder_opcode opcd) override;
    virtual bool is_div_opcode(decoder_opcode opcd) override;
    virtual bool is_pause_opcode(decoder_opcode opcd) override;
    virtual bool is_branch_opcode(decoder_opcode opcd) override;
    virtual bool is_fpvector_addsub_opcode(decoder_opcode opcd, const DecodedInst* ins) override;
    virtual bool is_fpvector_muldiv_opcode(decoder_opcode opcd, const DecodedInst* ins) override;
    virtual bool is_fpvector_ldst_opcode(decoder_opcode opcd, const DecodedInst* ins) override;
    virtual decoder_reg last_reg() override { return dl::last_reg; }
    virtual uint32_t map_register(decoder_reg reg) override { return reg; }
    virtual unsigned int num_read_implicit_registers(const DecodedInst *inst) override { return 0; }
    virtual decoder_reg get_read_implicit_reg(const DecodedInst* inst, unsigned int idx) override { return 0; }
    virtual unsigned int num_write_implicit_registers(const DecodedInst *inst) override { return 0; }
    virtual decoder_reg get_write_implicit_reg(const DecodedInst *inst, unsigned int idx) override { return 0; }
    virtual bool has_dynamic_memory_operands(const DecodedInst *ins) override;
    virtual void set_vector_bytes(unsigned int n) override { m_vector_bytes = n; }
    virtual void set_ext_memory_uops(unsigned int n) override { m_memory_uops = n < rvisa_max_memory_uops() ? n : rvisa_max_memory_uops(); }
    static unsigned int rvisa_max_memory_uops() { return 16; }   // Sift::MAX_MEMORY_SPANS

  private:
    unsigned int m_vector_bytes = 16;    /* static size of one vector memory uop */
    unsigned int m_memory_uops = 16;     /* load/store uops a footprint instruction spreads over */
  };

  class RISCVDecodedInst : public DecodedInst
  {
  public:
    RISCVDecodedInst(Decoder* d, const uint8_t * code, size_t size, uint64_t address);
    virtual unsigned int inst_num_id() const override { return m_op; }
    virtual std::string disassembly_to_str() const override;
    virtual bool is_nop() const override;
    virtual bool is_atomic() const override;
    virtual bool is_prefetch() const override;
    virtual bool is_serializing() const override;
    virtual bool is_conditional_branch() const override;
    virtual bool is_indirect_branch() const override;
    virtual bool is_barrier() const override;
    virtual bool src_dst_merge() const override { return false; }
    virtual bool is_X87() const override { return false; }
    virtual bool has_modifiers() const override { return false; }
    virtual bool is_mem_pair() const override { return false; }
    virtual bool is_writeback() const override { return false; }

    /* Filled by RISCVDecoder::decode. */
    unsigned int m_op = rvisa::OP_UNKNOWN;
    uint32_t m_word = 0;
    unsigned int m_length = 0;
    /* Register operands only, in template order (num_operands and friends). */
    struct Reg { uint8_t reg; bool read, write; };
    Reg m_regs[rvisa::MAX_FIELDS + 1];
    unsigned int m_nregs = 0;
    int m_base_reg = -1;        /* the xs1 of a memory instruction, or sp for the *sp forms */
    uint32_t flags() const { return rvisa::flags_of(m_op); }
  };
}
#endif
