#include "riscv_isa.h"

#include <stdio.h>
#include <string.h>

namespace rvisa
{

unsigned length_of(const uint8_t *b)
{
   if ((b[0] & 0x3) != 0x3) return 2;
   if ((b[0] & 0x1f) != 0x1f) return 4;
   return 0;
}

static unsigned lookup(uint32_t word, const uint16_t *start, const uint16_t *list, unsigned bucket)
{
   for (unsigned i = start[bucket]; i < start[bucket + 1]; ++i)
   {
      const Insn &in = insn_table[list[i]];
      if ((word & in.mask) == in.match)
         return list[i] + 1;
   }
   return OP_UNKNOWN;
}

unsigned decode(uint32_t word, unsigned length)
{
   if (length == 4)
      return lookup(word, bucket32_start, bucket32_list, (word >> 2) & 31);
   if (length == 2)
      return lookup(word & 0xffff, bucket16_start, bucket16_list, (word & 3) | (((word >> 13) & 7) << 2));
   return OP_UNKNOWN;
}

int64_t field_value(const Field &f, uint32_t word)
{
   uint64_t v = 0;
   unsigned width = 0;
   for (unsigned i = 0; i < f.nseg; ++i)
   {
      const unsigned n = f.seg[i].msb - f.seg[i].lsb + 1;
      v = (v << n) | ((word >> f.seg[i].lsb) & ((1u << n) - 1));
      width += n;
   }
   if (f.kind == F_XREG || f.kind == F_FREG || f.kind == F_VREG)
      return (int64_t)v + ((f.flags & FF_PLUS8) ? 8 : 0);
   v <<= f.shift;
   width += f.shift;
   if ((f.flags & FF_SEXT) && width < 64 && (v >> (width - 1)) & 1)
      v |= ~((1ull << width) - 1);
   return (int64_t)v;
}

unsigned operands(unsigned op, uint32_t word, Operand out[MAX_FIELDS + 1])
{
   if (!op) return 0;
   const Insn &in = insn(op);
   unsigned n = 0;
   bool masked = false;
   for (unsigned i = 0; i < in.nfields; ++i)
   {
      const Field &f = in.fields[i];
      out[n].name = f.name;
      out[n].kind = f.kind;
      out[n].flags = f.flags;
      out[n].value = field_value(f, word);
      if (f.kind == F_FREG) out[n].value += 32;
      if (f.kind == F_VREG) out[n].value += 64;
      if (f.kind == F_OTHER && strcmp(f.name, "vm") == 0 && out[n].value == 0)
         masked = true;
      n++;
   }
   if (masked && (info(in.cls).flags & FLAG_VECTOR))
   {
      out[n].name = "v0";
      out[n].kind = F_VREG;
      out[n].flags = FF_READ;
      out[n].value = 64;
      n++;
   }
   return n;
}

const char* xreg_name(unsigned r)
{
   static const char *const names[32] = {
      "zero", "ra", "sp", "gp", "tp", "t0", "t1", "t2", "s0", "s1", "a0", "a1",
      "a2", "a3", "a4", "a5", "a6", "a7", "s2", "s3", "s4", "s5", "s6", "s7",
      "s8", "s9", "s10", "s11", "t3", "t4", "t5", "t6" };
   return r < 32 ? names[r] : "?";
}

std::string disassemble(unsigned op, uint32_t word)
{
   if (!op)
   {
      char buf[32];
      snprintf(buf, sizeof(buf), ".word 0x%08x", word);
      return buf;
   }
   const Insn &in = insn(op);
   Operand ops[MAX_FIELDS + 1];
   const unsigned n = operands(op, word, ops);
   std::string s = in.name;
   while (s.length() < 12) s += " ";
   /* Substitute each operand name in the UDB assembly template. */
   const char *t = in.assembly;
   while (*t)
   {
      if ((*t >= 'a' && *t <= 'z') || (*t >= 'A' && *t <= 'Z'))
      {
         const char *e = t;
         while ((*e >= 'a' && *e <= 'z') || (*e >= 'A' && *e <= 'Z') || (*e >= '0' && *e <= '9') || *e == '_') e++;
         std::string tok(t, e - t);
         bool done = false;
         for (unsigned i = 0; i < n && !done; ++i)
         {
            if (tok != ops[i].name) continue;
            char buf[32];
            const int64_t v = ops[i].value;
            if (ops[i].kind == F_XREG)       snprintf(buf, sizeof(buf), "%s", xreg_name((unsigned)v));
            else if (ops[i].kind == F_FREG)  snprintf(buf, sizeof(buf), "f%d", (int)(v - 32));
            else if (ops[i].kind == F_VREG)  snprintf(buf, sizeof(buf), "v%d", (int)(v - 64));
            else if (ops[i].kind == F_CSR)   snprintf(buf, sizeof(buf), "0x%x", (unsigned)v);
            else                             snprintf(buf, sizeof(buf), "%lld", (long long)v);
            s += buf;
            done = true;
         }
         if (!done) s += tok;      /* sp, vm, rm, ...: literal */
         t = e;
      }
      else
         s += *t++;
   }
   return s;
}

bool self_test(std::string *why)
{
   for (unsigned i = 0; i < NUM_INSNS; ++i)
   {
      const Insn &in = insn_table[i];
      const unsigned got = decode(in.match, in.length / 8);
      if (got != (unsigned)expected_decode[i] + 1)
      {
         if (why)
         {
            char buf[256];
            snprintf(buf, sizeof(buf), "sample word of %s (0x%08x, %u bits) decodes as %s, expected %s",
                     in.name, in.match, in.length, name(got), insn_table[expected_decode[i]].name);
            *why = buf;
         }
         return false;
      }
   }
   return true;
}

}
