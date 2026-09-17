#include "riscv_decoder.h"

#include <set>
#include <stdio.h>
#include <string.h>

namespace dl
{

RISCVDecoder::RISCVDecoder(dl_arch arch, dl_mode mode, dl_syntax syntax)
{
  this->m_arch = arch;
  this->m_mode = mode;
  this->m_syntax = syntax;
  std::string why;
  if (!rvisa::self_test(&why))
    fprintf(stderr, "[RISCV] generated instruction table failed its self-test: %s\n", why.c_str());
}

RISCVDecoder::~RISCVDecoder() {}

void RISCVDecoder::decode(DecodedInst * inst)
{
  if (inst->get_already_decoded())
    return;
  RISCVDecodedInst *d = (RISCVDecodedInst *)inst;
  const uint8_t *code = inst->get_code();
  d->m_length = rvisa::length_of(code);
  d->m_word = rvisa::word_of(code, d->m_length);
  d->m_op = rvisa::decode(d->m_word, d->m_length);
  if (d->m_op == rvisa::OP_UNKNOWN)
  {
    /* Not in the generated tables: say so once per encoding; the core
       model costs it as a generic instruction. */
    static std::set<uint32_t> reported;
    if (reported.insert(d->m_word).second)
      fprintf(stderr, "[RISCV] undecoded instruction 0x%08x (%u bytes): costed as generic\n", d->m_word, d->m_length);
  }
  rvisa::Operand ops[rvisa::MAX_FIELDS + 1];
  const unsigned n = rvisa::operands(d->m_op, d->m_word, ops);
  d->m_nregs = 0;
  d->m_base_reg = -1;
  for (unsigned i = 0; i < n; ++i)
  {
    if (ops[i].kind != rvisa::F_XREG && ops[i].kind != rvisa::F_FREG && ops[i].kind != rvisa::F_VREG)
      continue;
    RISCVDecodedInst::Reg &r = d->m_regs[d->m_nregs++];
    r.reg = (uint8_t)ops[i].value;
    r.read = (ops[i].flags & rvisa::FF_READ) != 0;
    r.write = (ops[i].flags & rvisa::FF_WRITE) != 0;
    if (strcmp(ops[i].name, "xs1") == 0)
      d->m_base_reg = (int)ops[i].value;
  }
  if (d->m_base_reg < 0 && (d->flags() & (rvisa::FLAG_LOAD | rvisa::FLAG_STORE)))
    d->m_base_reg = 2;          /* c.lwsp, c.sdsp, ...: the stack pointer is implicit */
  inst->set_already_decoded(true);
}

void RISCVDecoder::decode(DecodedInst * inst, dl_isa isa) { decode(inst); }
void RISCVDecoder::change_isa_mode(dl_isa new_isa) {}

const char* RISCVDecoder::inst_name(unsigned int inst_id) { return rvisa::name(inst_id); }

const char* RISCVDecoder::reg_name(unsigned int reg_id)
{
  static char buf[8];
  if (reg_id < 32) return rvisa::xreg_name(reg_id);
  if (reg_id < 64) { snprintf(buf, sizeof(buf), "f%u", reg_id - 32); return buf; }
  if (reg_id < 96) { snprintf(buf, sizeof(buf), "v%u", reg_id - 64); return buf; }
  return "?";
}

Decoder::decoder_reg RISCVDecoder::largest_enclosing_register(decoder_reg r) { return r; }
bool RISCVDecoder::invalid_register(decoder_reg r) { return r >= dl::last_reg; }
bool RISCVDecoder::reg_is_program_counter(decoder_reg r) { return false; }
bool RISCVDecoder::inst_in_group(const DecodedInst * inst, unsigned int group_id) { return false; }

static inline const RISCVDecodedInst* R(const DecodedInst *inst) { return (const RISCVDecodedInst *)inst; }

unsigned int RISCVDecoder::num_operands(const DecodedInst * inst) { return R(inst)->m_nregs; }
bool RISCVDecoder::op_read_reg(const DecodedInst * inst, unsigned int idx) { return idx < R(inst)->m_nregs && R(inst)->m_regs[idx].read; }
bool RISCVDecoder::op_write_reg(const DecodedInst * inst, unsigned int idx) { return idx < R(inst)->m_nregs && R(inst)->m_regs[idx].write; }
bool RISCVDecoder::op_is_reg(const DecodedInst * inst, unsigned int idx) { return idx < R(inst)->m_nregs; }
Decoder::decoder_reg RISCVDecoder::get_op_reg(const DecodedInst * inst, unsigned int idx) { return idx < R(inst)->m_nregs ? R(inst)->m_regs[idx].reg : 0; }

unsigned int RISCVDecoder::num_memory_operands(const DecodedInst * inst)
{
  const uint32_t f = R(inst)->flags();
  if (!(f & (rvisa::FLAG_LOAD | rvisa::FLAG_STORE)))
    return 0;
  /* A recorded footprint (vector, matrix tile) can spread over many uops;
     the spare ones are squashed at run time. */
  return (f & rvisa::FLAG_FOOTPRINT) ? m_memory_uops : 1;
}

Decoder::decoder_reg RISCVDecoder::mem_base_reg(const DecodedInst * inst, unsigned int mem_idx)
{
  return R(inst)->m_base_reg >= 0 ? R(inst)->m_base_reg : 0;
}
Decoder::decoder_reg RISCVDecoder::mem_index_reg(const DecodedInst * inst, unsigned int mem_idx) { return mem_base_reg(inst, mem_idx); }
bool RISCVDecoder::op_read_mem(const DecodedInst * inst, unsigned int mem_idx) { return (R(inst)->flags() & rvisa::FLAG_LOAD) != 0; }
bool RISCVDecoder::op_write_mem(const DecodedInst * inst, unsigned int mem_idx) { return (R(inst)->flags() & rvisa::FLAG_STORE) != 0; }

unsigned int RISCVDecoder::size_mem_op(const DecodedInst * inst, unsigned int mem_idx)
{
  const RISCVDecodedInst *d = R(inst);
  if (d->flags() & rvisa::FLAG_FOOTPRINT)
    return m_vector_bytes;
  return d->m_op ? rvisa::insn(d->m_op).mem_size : 0;
}

uint16_t RISCVDecoder::get_operand_size(const DecodedInst *ins)
{
  return (R(ins)->flags() & rvisa::FLAG_VECTOR) ? m_vector_bytes * 8 : 64;
}

bool RISCVDecoder::is_cache_flush_opcode(decoder_opcode opcd) { return rvisa::has(opcd, rvisa::FLAG_CACHE_FLUSH); }
bool RISCVDecoder::is_div_opcode(decoder_opcode opcd) { return rvisa::has(opcd, rvisa::FLAG_BUSY); }
bool RISCVDecoder::is_pause_opcode(decoder_opcode opcd) { return opcd && strcmp(rvisa::name(opcd), "pause") == 0; }
bool RISCVDecoder::is_branch_opcode(decoder_opcode opcd) { return rvisa::has(opcd, rvisa::FLAG_BRANCH); }
bool RISCVDecoder::is_fpvector_addsub_opcode(decoder_opcode opcd, const DecodedInst* ins) { return opcd && rvisa::info(rvisa::class_of(opcd)).cpi == rvisa::CPI_FP_ADDSUB; }
bool RISCVDecoder::is_fpvector_muldiv_opcode(decoder_opcode opcd, const DecodedInst* ins) { return opcd && rvisa::info(rvisa::class_of(opcd)).cpi == rvisa::CPI_FP_MULDIV; }
bool RISCVDecoder::is_fpvector_ldst_opcode(decoder_opcode opcd, const DecodedInst* ins) { return opcd && rvisa::info(rvisa::class_of(opcd)).cpi == rvisa::CPI_LDST; }
bool RISCVDecoder::has_dynamic_memory_operands(const DecodedInst *ins) { return (R(ins)->flags() & rvisa::FLAG_FOOTPRINT) != 0; }

RISCVDecodedInst::RISCVDecodedInst(Decoder* d, const uint8_t * code, size_t size, uint64_t address)
{
  this->m_dec = d;
  this->m_code = code;
  this->m_size = size;
  this->m_address = address;
  this->m_already_decoded = false;
}

std::string RISCVDecodedInst::disassembly_to_str() const { return rvisa::disassemble(m_op, m_word); }

bool RISCVDecodedInst::is_nop() const
{
  if (!m_op) return false;
  const char *n = rvisa::name(m_op);
  if (strcmp(n, "c.nop") == 0) return true;
  return strcmp(n, "addi") == 0 && (m_word & 0xffffff80) == 0;   /* addi x0, x0, 0 */
}
bool RISCVDecodedInst::is_atomic() const { return (flags() & rvisa::FLAG_ATOMIC) != 0; }
bool RISCVDecodedInst::is_prefetch() const { return (flags() & rvisa::FLAG_PREFETCH) != 0; }
bool RISCVDecodedInst::is_serializing() const { return (flags() & rvisa::FLAG_SERIALIZING) != 0; }
bool RISCVDecodedInst::is_conditional_branch() const { return (flags() & rvisa::FLAG_BRANCH) != 0; }
bool RISCVDecodedInst::is_indirect_branch() const { return (flags() & rvisa::FLAG_INDIRECT) != 0; }
bool RISCVDecodedInst::is_barrier() const { return m_op && strncmp(rvisa::name(m_op), "fence", 5) == 0; }

}
