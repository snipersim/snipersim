/* RISC-V vector (RVV 1.0) and Integrated Matrix Extension (IME / Zvvm 0.9)
 * classifier.
 *
 * rv8, the decoder Sniper's RISC-V support is built on, predates both
 * extensions and reports their encodings as illegal.  This module recognises
 * them from the raw instruction word and assigns each an opcode id that
 * continues rv8's numbering (OP_BASE == rv8's rv_op_last, checked where the
 * two meet), so the core models can index their latency tables with either.
 *
 * It depends on nothing but the C++ standard library: the QEMU frontend
 * compiles it too, to know which instructions need their full memory
 * footprint recorded (see Sift::Writer::MemorySpans). */
#ifndef RISCV_EXT_H
#define RISCV_EXT_H

#include <stdint.h>
#include <string>

namespace rvext
{
   enum Class
   {
      NONE = 0,
      V_CFG,            /* vsetvli, vsetivli, vsetvl */
      V_LOAD,           /* vector loads: unit-stride, strided, indexed, whole-register, mask */
      V_STORE,          /* vector stores */
      V_INT_ALU,        /* integer, mask and permutation arithmetic */
      V_INT_MUL,        /* integer multiply and multiply-add */
      V_INT_DIV,        /* integer divide and remainder */
      V_FP_ADDSUB,      /* FP add, sub, min/max, compare, convert, sign-inject */
      V_FP_MULADD,      /* FP multiply and fused multiply-add */
      V_FP_DIV,         /* FP divide, square root and reciprocal estimates */
      IME_CFG,          /* vsetlambda */
      IME_TILE_LOAD,    /* vmtl.v, vmttl.v */
      IME_TILE_STORE,   /* vmts.v, vmtts.v */
      IME_MMA_INT,      /* vmmacc.vv family */
      IME_MMA_FP,       /* vfmmacc.vv family, including the microscaled forms */
      NUM_CLASSES
   };

   /* Opcode ids.  OP_BASE continues rv8's enumeration. */
   const unsigned OP_BASE = 319;
   enum Op
   {
      OP_NONE = 0,
      OP_FIRST = OP_BASE,
      /* IME 0.9 (Zvvmm, Zvvfmm, Zvvmtls, Zvvmttls) */
      OP_VMMACC_VV = OP_FIRST, OP_VWMMACC_VV, OP_VQMMACC_VV, OP_V8WMMACC_VV,
      OP_VFMMACC_VV, OP_VFWMMACC_VV, OP_VFQMMACC_VV, OP_VF8WMMACC_VV,
      OP_VFWIMMACC_VV, OP_VFQIMMACC_VV, OP_VF8WIMMACC_VV,
      OP_VMTL_V, OP_VMTS_V, OP_VMTTL_V, OP_VMTTS_V, OP_VSETLAMBDA,
      /* RVV 1.0 */
      OP_VSETVLI, OP_VSETIVLI, OP_VSETVL,
      OP_VLE, OP_VLUXEI, OP_VLSE, OP_VLOXEI,
      OP_VSE, OP_VSUXEI, OP_VSSE, OP_VSOXEI,
      OP_VIALU, OP_VIMUL, OP_VIDIV, OP_VFADD, OP_VFMUL, OP_VFDIV,
      OP_LAST
   };

   struct OpInfo
   {
      const char *name;
      Class cls;
   };

   /* Register numbering shared with dl::RISCVDecoder's reg_num: x0-x31 are
    * 0-31, f0-f31 are 32-63, v0-v31 are 64-95. */
   struct Operand
   {
      uint8_t reg;
      bool read;
      bool write;
   };
   const unsigned MAX_OPERANDS = 5;

   /* The opcode id of a 32-bit instruction word, or OP_NONE when it is not a
    * vector or matrix instruction. */
   unsigned decode(uint32_t word);

   inline bool is_ext(unsigned op) { return op >= OP_FIRST && op < OP_LAST; }
   const OpInfo& info(unsigned op);
   Class class_of(unsigned op);
   const char* name(unsigned op);
   const char* class_name(Class c);
   bool is_load(unsigned op);
   bool is_store(unsigned op);
   inline bool is_memory(unsigned op) { return is_load(op) || is_store(op); }
   bool is_fp(unsigned op);

   /* The register operands of the instruction, in the order the assembly
    * syntax lists them.  Returns how many were written to out[]. */
   unsigned operands(uint32_t word, unsigned op, Operand out[MAX_OPERANDS]);
   /* The integer register holding a memory instruction's base address. */
   inline unsigned base_reg(uint32_t word) { return (word >> 15) & 0x1f; }

   std::string disassemble(uint32_t word, unsigned op);
}

#endif
