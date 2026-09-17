#include "riscv_ext.h"

#include <stdio.h>

namespace rvext
{

namespace
{
   /* IME 0.9 encodings, as the extension's own disassembler lists them
    * (ime-0.9-pipeline/generated/trace/ime_disas.c).  They sit in OP-V,
    * LOAD-FP/STORE-FP and custom-0 space that RVV 1.0 leaves reserved, so
    * they are tried before the RVV classification. */
   struct Pattern { uint32_t mask, match; unsigned op; };
   const Pattern ime_patterns[] = {
      { 0xfe00707f, 0xec000057, OP_VF8WIMMACC_VV },
      { 0xfc00707f, 0x5c001057, OP_VF8WMMACC_VV },
      { 0xfc00707f, 0x50001057, OP_VFMMACC_VV },
      { 0xfe00707f, 0xe8000057, OP_VFQIMMACC_VV },
      { 0xfc00707f, 0x58001057, OP_VFQMMACC_VV },
      { 0xfe00707f, 0xe4000057, OP_VFWIMMACC_VV },
      { 0xfc00707f, 0x54001057, OP_VFWMMACC_VV },
      { 0xfe00707f, 0xee000057, OP_V8WMMACC_VV },
      { 0xfc00707f, 0xe0000057, OP_VMMACC_VV },
      { 0xfe00707f, 0xea000057, OP_VQMMACC_VV },
      { 0xfe00707f, 0xe6000057, OP_VWMMACC_VV },
      { 0x1c00707f, 0x10007007, OP_VMTL_V },
      { 0x1c00707f, 0x10007027, OP_VMTS_V },
      { 0x1c00707f, 0x14007007, OP_VMTTL_V },
      { 0x1c00707f, 0x14007027, OP_VMTTS_V },
      { 0x0000707f, 0x0000000b, OP_VSETLAMBDA },
   };

   const OpInfo op_table[OP_LAST - OP_FIRST] = {
      { "vmmacc.vv",     IME_MMA_INT },
      { "vwmmacc.vv",    IME_MMA_INT },
      { "vqmmacc.vv",    IME_MMA_INT },
      { "v8wmmacc.vv",   IME_MMA_INT },
      { "vfmmacc.vv",    IME_MMA_FP },
      { "vfwmmacc.vv",   IME_MMA_FP },
      { "vfqmmacc.vv",   IME_MMA_FP },
      { "vf8wmmacc.vv",  IME_MMA_FP },
      { "vfwimmacc.vv",  IME_MMA_FP },
      { "vfqimmacc.vv",  IME_MMA_FP },
      { "vf8wimmacc.vv", IME_MMA_FP },
      { "vmtl.v",        IME_TILE_LOAD },
      { "vmts.v",        IME_TILE_STORE },
      { "vmttl.v",       IME_TILE_LOAD },
      { "vmtts.v",       IME_TILE_STORE },
      { "vsetlambda",    IME_CFG },
      { "vsetvli",       V_CFG },
      { "vsetivli",      V_CFG },
      { "vsetvl",        V_CFG },
      { "vle",           V_LOAD },
      { "vluxei",        V_LOAD },
      { "vlse",          V_LOAD },
      { "vloxei",        V_LOAD },
      { "vse",           V_STORE },
      { "vsuxei",        V_STORE },
      { "vsse",          V_STORE },
      { "vsoxei",        V_STORE },
      { "vop.ialu",      V_INT_ALU },
      { "vop.imul",      V_INT_MUL },
      { "vop.idiv",      V_INT_DIV },
      { "vop.fadd",      V_FP_ADDSUB },
      { "vop.fmul",      V_FP_MULADD },
      { "vop.fdiv",      V_FP_DIV },
   };

   const char *const class_names[NUM_CLASSES] = {
      "none", "v_cfg", "v_load", "v_store", "v_int_alu", "v_int_mul", "v_int_div",
      "v_fp_addsub", "v_fp_muladd", "v_fp_div",
      "ime_cfg", "ime_tile_load", "ime_tile_store", "ime_mma_int", "ime_mma_fp",
   };
   const OpInfo none_info = { "(none)", NONE };

   const char *const xreg_names[32] = {
      "zero", "ra", "sp", "gp", "tp", "t0", "t1", "t2", "s0", "s1", "a0", "a1",
      "a2", "a3", "a4", "a5", "a6", "a7", "s2", "s3", "s4", "s5", "s6", "s7",
      "s8", "s9", "s10", "s11", "t3", "t4", "t5", "t6" };

   inline uint8_t X(unsigned r) { return (uint8_t)r; }
   inline uint8_t F(unsigned r) { return (uint8_t)(32 + r); }
   inline uint8_t V(unsigned r) { return (uint8_t)(64 + r); }

   inline Operand make(uint8_t reg, bool read, bool write)
   {
      Operand o; o.reg = reg; o.read = read; o.write = write; return o;
   }
}

unsigned decode(uint32_t w)
{
   for (const Pattern &p : ime_patterns)
      if ((w & p.mask) == p.match)
         return p.op;

   const uint32_t opc = w & 0x7f;
   const uint32_t f3 = (w >> 12) & 7;

   if (opc == 0x57)                     /* OP-V */
   {
      if (f3 == 7)                      /* vsetvl family */
      {
         if ((w >> 31) == 0)            return OP_VSETVLI;
         if (((w >> 30) & 3) == 3)      return OP_VSETIVLI;
         if (((w >> 25) & 0x7f) == 0x40) return OP_VSETVL;
         return OP_NONE;
      }
      const uint32_t f6 = w >> 26;
      switch (f3)
      {
         case 0: case 3: case 4:        /* OPIVV, OPIVI, OPIVX */
            /* funct6 0x27 is vsmul (IVV/IVX) and vmv<nr>r (IVI). */
            return (f6 == 0x27 && f3 != 3) ? OP_VIMUL : OP_VIALU;
         case 2: case 6:                /* OPMVV, OPMVX */
            if (f6 >= 0x20 && f6 <= 0x23)
               return OP_VIDIV;         /* vdivu, vdiv, vremu, vrem */
            if ((f6 >= 0x24 && f6 <= 0x27) || f6 == 0x29 || f6 == 0x2b
                || f6 == 0x2d || f6 == 0x2f || f6 >= 0x38)
               return OP_VIMUL;         /* vmul*, vmadd/vnmsub/vmacc/vnmsac, vwmul*, vwmacc* */
            return OP_VIALU;            /* reductions, averaging, mask ops, widening add/sub */
         case 1: case 5:                /* OPFVV, OPFVF */
            if (f6 == 0x20 || f6 == 0x21)
               return OP_VFDIV;         /* vfdiv, vfrdiv */
            if (f6 == 0x13)             /* VFUNARY1 */
            {
               const uint32_t vs1 = (w >> 15) & 0x1f;
               if (vs1 == 0 || vs1 == 4 || vs1 == 5)
                  return OP_VFDIV;      /* vfsqrt, vfrsqrt7, vfrec7 */
               return OP_VFADD;         /* vfclass */
            }
            if (f6 == 0x24 || (f6 >= 0x28 && f6 <= 0x2f) || f6 == 0x38 || f6 >= 0x3c)
               return OP_VFMUL;         /* vfmul, the FMA forms, vfwmul, vfwmacc* */
            return OP_VFADD;
         default:
            return OP_NONE;
      }
   }

   if (opc == 0x07 || opc == 0x27)      /* LOAD-FP / STORE-FP */
   {
      if (!(f3 == 0 || f3 >= 5))
         return OP_NONE;                /* widths 1-4 are the scalar FP loads and stores */
      if ((w >> 28) & 1)
         return OP_NONE;                /* mew=1 is reserved in RVV; the IME tiles above use it */
      const uint32_t mop = (w >> 26) & 3;
      static const unsigned ld[4] = { OP_VLE, OP_VLUXEI, OP_VLSE, OP_VLOXEI };
      static const unsigned st[4] = { OP_VSE, OP_VSUXEI, OP_VSSE, OP_VSOXEI };
      return opc == 0x07 ? ld[mop] : st[mop];
   }

   return OP_NONE;
}

const OpInfo& info(unsigned op)
{
   return is_ext(op) ? op_table[op - OP_FIRST] : none_info;
}

Class class_of(unsigned op) { return info(op).cls; }
const char* name(unsigned op) { return info(op).name; }

const char* class_name(Class c)
{
   return (c >= 0 && c < NUM_CLASSES) ? class_names[c] : "?";
}

bool is_load(unsigned op)
{
   const Class c = class_of(op);
   return c == V_LOAD || c == IME_TILE_LOAD;
}

bool is_store(unsigned op)
{
   const Class c = class_of(op);
   return c == V_STORE || c == IME_TILE_STORE;
}

bool is_fp(unsigned op)
{
   const Class c = class_of(op);
   return c == V_FP_ADDSUB || c == V_FP_MULADD || c == V_FP_DIV || c == IME_MMA_FP;
}

unsigned operands(uint32_t w, unsigned op, Operand out[MAX_OPERANDS])
{
   const unsigned rd = (w >> 7) & 0x1f, rs1 = (w >> 15) & 0x1f, rs2 = (w >> 20) & 0x1f;
   const bool vm = (w >> 25) & 1;
   const uint32_t f3 = (w >> 12) & 7;
   unsigned n = 0;

   switch (class_of(op))
   {
      case IME_MMA_INT:
      case IME_MMA_FP:
         out[n++] = make(V(rd), true, true);      /* C accumulates in place */
         out[n++] = make(V(rs1), true, false);
         out[n++] = make(V(rs2), true, false);
         if (!vm) out[n++] = make(V(0), true, false);   /* microscaling: E8M0 scales in v0 */
         break;
      case IME_TILE_LOAD:
      case IME_TILE_STORE:
         out[n++] = make(V(rd), class_of(op) == IME_TILE_STORE, class_of(op) == IME_TILE_LOAD);
         out[n++] = make(X(rs1), true, false);
         out[n++] = make(X(rs2), true, false);
         if (!vm) out[n++] = make(V(0), true, false);
         break;
      case IME_CFG:
         out[n++] = make(X(rd), false, true);
         out[n++] = make(X(rs1), true, false);
         break;
      case V_CFG:
         out[n++] = make(X(rd), false, true);
         if (op != OP_VSETIVLI) out[n++] = make(X(rs1), true, false);
         if (op == OP_VSETVL)   out[n++] = make(X(rs2), true, false);
         break;
      case V_LOAD:
      case V_STORE:
      {
         const uint32_t mop = (w >> 26) & 3;
         out[n++] = make(V(rd), class_of(op) == V_STORE, class_of(op) == V_LOAD);
         out[n++] = make(X(rs1), true, false);
         if (mop == 2)      out[n++] = make(X(rs2), true, false);   /* strided */
         else if (mop & 1)  out[n++] = make(V(rs2), true, false);   /* indexed */
         if (!vm) out[n++] = make(V(0), true, false);
         break;
      }
      case V_INT_ALU: case V_INT_MUL: case V_INT_DIV:
      case V_FP_ADDSUB: case V_FP_MULADD: case V_FP_DIV:
      {
         const Class c = class_of(op);
         const bool accumulates = (c == V_INT_MUL || c == V_FP_MULADD);
         out[n++] = make(V(rd), accumulates, true);
         out[n++] = make(V(rs2), true, false);
         if (f3 == 0 || f3 == 1 || f3 == 2)       out[n++] = make(V(rs1), true, false);   /* .vv */
         else if (f3 == 4 || f3 == 6)             out[n++] = make(X(rs1), true, false);   /* .vx */
         else if (f3 == 5)                        out[n++] = make(F(rs1), true, false);   /* .vf */
         /* f3 == 3: .vi, immediate */
         if (!vm) out[n++] = make(V(0), true, false);
         break;
      }
      default:
         break;
   }
   return n;
}

std::string disassemble(uint32_t w, unsigned op)
{
   Operand ops[MAX_OPERANDS];
   const unsigned n = operands(w, op, ops);
   std::string s = name(op);
   while (s.length() < 12) s += " ";
   for (unsigned i = 0; i < n; ++i)
   {
      char buf[16];
      const unsigned r = ops[i].reg;
      if (r < 32)       snprintf(buf, sizeof(buf), "%s", xreg_names[r]);
      else if (r < 64)  snprintf(buf, sizeof(buf), "f%u", r - 32);
      else              snprintf(buf, sizeof(buf), "v%u", r - 64);
      if (i) s += ", ";
      s += buf;
   }
   return s;
}

}
