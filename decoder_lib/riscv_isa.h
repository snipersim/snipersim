/* The generated RISC-V instruction tables (riscv_isa_gen.{h,cc}, produced by
 * tools/riscv/gen_isa.py from the RISC-V Unified Database) and the small API
 * over them that Sniper's decoder, its core models and the QEMU frontend
 * share.  Nothing here depends on the simulator, rv8 or QEMU. */
#ifndef RISCV_ISA_H
#define RISCV_ISA_H

#include <stdint.h>
#include <string>
#include "riscv_isa_gen.h"

namespace rvisa
{
   struct Seg { uint8_t msb, lsb; };
   struct Field
   {
      const char *name;          /* xd, xs1, fd, vd, vs3, imm, csr, vm, ... */
      FieldKind kind;
      uint8_t flags;             /* FF_READ | FF_WRITE | FF_PLUS8 | FF_SEXT */
      uint8_t nseg;
      uint8_t shift;             /* left shift after concatenating the segments */
      Seg seg[MAX_SEGS];         /* most significant segment first */
   };
   struct Insn
   {
      const char *name;
      const char *ext;           /* the first extension the UDB lists as defining it */
      const char *assembly;      /* the UDB assembly template, e.g. "xd, imm(xs1)" */
      uint32_t mask, match;
      uint8_t length;            /* 16 or 32 bits */
      Class cls;
      uint8_t nfields;
      uint8_t mem_size;          /* bytes moved by a scalar memory instruction, else 0 */
      Field fields[MAX_FIELDS];
   };
   struct ClassInfo
   {
      const char *name;
      const char *cfg_key;       /* perf_model/core/riscv/<class>; _latency and _issue_cycles hang off it */
      uint16_t latency;          /* placeholder, cycles */
      uint16_t issue_cycles;
      Cpi cpi;
      uint32_t flags;            /* FLAG_* */
   };

   extern const ClassInfo class_table[NUM_CLASSES];
   extern const Insn insn_table[NUM_INSNS];
   extern const uint16_t bucket32_start[33], bucket32_list[], bucket16_start[33], bucket16_list[];
   extern const uint16_t expected_decode[NUM_INSNS];

   /* Opcode ids: 0 is "not an instruction we know", 1..NUM_INSNS index the table. */
   const unsigned OP_UNKNOWN = 0;
   const unsigned NUM_OPS = NUM_INSNS + 1;

   /* Instruction length in bytes from its first two bytes: 2, 4, or 0 for the
    * longer encodings the base spec reserves. */
   unsigned length_of(const uint8_t *bytes);
   inline uint32_t word_of(const uint8_t *bytes, unsigned length)
   {
      uint32_t w = (uint32_t)bytes[0] | ((uint32_t)bytes[1] << 8);
      if (length == 4) w |= ((uint32_t)bytes[2] << 16) | ((uint32_t)bytes[3] << 24);
      return w;
   }
   unsigned decode(uint32_t word, unsigned length);      /* opcode id */

   inline const Insn& insn(unsigned op) { return insn_table[op - 1]; }
   inline Class class_of(unsigned op) { return op ? insn(op).cls : (Class)0; }
   inline const ClassInfo& info(Class c) { return class_table[c]; }
   inline uint32_t flags_of(unsigned op) { return op ? info(insn(op).cls).flags : 0; }
   inline bool has(unsigned op, Flag f) { return (flags_of(op) & f) != 0; }
   inline const char* name(unsigned op) { return op ? insn(op).name : "unknown"; }

   /* A decoded operand: for registers `value` is the register number in the
    * shared numbering x0-31 = 0-31, f0-31 = 32-63, v0-31 = 64-95, AME m0-31 =
    * 96-127 and accumulators a0-15 = 128-143; for immediates the shifted,
    * sign-extended value. */
   struct Operand
   {
      const char *name;
      FieldKind kind;
      uint8_t flags;             /* FF_READ / FF_WRITE for registers */
      int64_t value;
   };
   int64_t field_value(const Field &f, uint32_t word);
   /* All fields of the instruction as operands, plus v0 as a read operand
    * when a vector instruction is masked (vm = 0).  Returns the count. */
   unsigned operands(unsigned op, uint32_t word, Operand out[MAX_FIELDS + 1]);
   const char* xreg_name(unsigned r);
   std::string disassemble(unsigned op, uint32_t word);

   /* Every table row's sample word decodes to the row the generator
    * expected; false with a reason otherwise.  Cheap: run it at startup. */
   bool self_test(std::string *why);
}

#endif
