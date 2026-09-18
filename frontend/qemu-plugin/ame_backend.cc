#include "ame_backend.h"
#include "riscv_isa.h"

#include <string.h>
#include <stdio.h>

namespace ame
{

unsigned square_n = 4;

namespace
{
   struct Ops { int md = -1, ms1 = -1, ms2 = -1, ad = -1, as1 = -1, xd = -1, xs1 = -1, xs2 = -1; };

   Ops operands(unsigned op, uint32_t word)
   {
      Ops o;
      rvisa::Operand ops[rvisa::MAX_FIELDS + 1];
      unsigned n = rvisa::operands(op, word, ops);
      for (unsigned i = 0; i < n; i++)
      {
         const int v = (int)ops[i].value;
         const char *nm = ops[i].name;
         if (!strcmp(nm, "md"))  o.md  = v - 96;
         if (!strcmp(nm, "ms1")) o.ms1 = v - 96;
         if (!strcmp(nm, "ms2")) o.ms2 = v - 96;
         if (!strcmp(nm, "ad"))  o.ad  = v - 128;
         if (!strcmp(nm, "as1")) o.as1 = v - 128;
         if (!strcmp(nm, "xd"))  o.xd  = v;
         if (!strcmp(nm, "xs1")) o.xs1 = v;
         if (!strcmp(nm, "xs2")) o.xs2 = v;
      }
      return o;
   }

   bool square_transfer(Backend &b, const Env &env, int32_t *sq, bool store, bool colmajor, bool strided, bool transposed_stride,
                        uint64_t base, uint64_t stride)
   {
      const unsigned N = square_n, NELEM = N * N;
      /* Row-major: element (i,j) at index i*N+j of a contiguous 16-element string;
       * column-major: (i,j) at j*N+i.  Strided (.st): row i is a segment at
       * base + i*stride; transposed-strided (.tst): column j is the segment. */
      int32_t buf[MAXN * MAXN];
      if (store)
         memcpy(buf, sq, NELEM * sizeof(int32_t));
      if (!strided)
      {
         int32_t mem[MAXN * MAXN];
         if (store)
            for (unsigned i = 0; i < N; i++) for (unsigned j = 0; j < N; j++) mem[colmajor ? j * N + i : i * N + j] = buf[i * N + j];
         if (store ? !env.write_mem(base, mem, NELEM * 4) : !env.read_mem(base, mem, NELEM * 4))
            return false;
         env.note_access(base, NELEM * 4, store);
         if (!store)
            for (unsigned i = 0; i < N; i++) for (unsigned j = 0; j < N; j++) buf[i * N + j] = mem[colmajor ? j * N + i : i * N + j];
      }
      else
      {
         for (unsigned s = 0; s < N; s++)
         {
            int32_t seg[MAXN];
            const uint64_t addr = base + s * stride;
            if (store)
               for (unsigned e = 0; e < N; e++) seg[e] = transposed_stride ? buf[e * N + s] : buf[s * N + e];
            if (store ? !env.write_mem(addr, seg, N * 4) : !env.read_mem(addr, seg, N * 4))
               return false;
            env.note_access(addr, N * 4, store);
            if (!store)
               for (unsigned e = 0; e < N; e++) { if (transposed_stride) buf[e * N + s] = seg[e]; else buf[s * N + e] = seg[e]; }
         }
      }
      if (!store)
         memcpy(sq, buf, NELEM * sizeof(int32_t));
      return true;
   }
}

bool execute(Backend &b, unsigned op, uint32_t word, const Env &env)
{
   const unsigned N = square_n, NELEM = N * N;
   const char *name = rvisa::name(op);
   const Ops o = operands(op, word);
   auto fail = [&](const std::string &why) { b.errors++; env.error(std::string(name) + ": " + why); return false; };
   b.executed++;

   if (!strcmp(name, "ame.acquire"))
   {
      /* Whole backend, one owner per hart in this model: TRY succeeds. */
      b.owned = true;
      if (o.xd > 0) env.write_x(o.xd, 1);        /* SUCCESS=1, STATUS=0 */
      return true;
   }
   if (!strcmp(name, "ame.release")) { b.owned = false; return true; }
   if (!b.owned)
      return fail("executed without owning the backend (the specification raises illegal-instruction)");

   auto dtype_ok = [&](uint32_t d) { return d == DTYPE_INT32 || d == DTYPE_FP32; };
   auto as_f = [](int32_t v) { float f; memcpy(&f, &v, 4); return f; };
   auto as_i = [](float f) { int32_t v; memcpy(&v, &f, 4); return v; };
   if (!strcmp(name, "asettyp"))
   {
      const uint32_t d = (uint32_t)env.read_x(o.xs1);
      if (!dtype_ok(d)) return fail("unsupported accumulator datatype descriptor");
      b.Ad[o.ad] = d; memset(b.Acc[o.ad], 0, sizeof(b.Acc[o.ad])); return true;
   }
   if (!strcmp(name, "msettyp"))
   {
      const uint32_t d = (uint32_t)env.read_x(o.xs1);
      if (!dtype_ok(d)) return fail("unsupported M-register datatype descriptor");
      b.Md[o.md] = d; memset(b.M[o.md], 0, sizeof(b.M[o.md])); return true;
   }
   if (!strcmp(name, "agettyp")) { if (o.xd > 0) env.write_x(o.xd, b.Ad[o.as1 >= 0 ? o.as1 : o.ad]); return true; }
   if (!strcmp(name, "mgettyp")) { if (o.xd > 0) env.write_x(o.xd, b.Md[o.ms1]); return true; }

   /* memory */
   if (!strncmp(name, "mls.", 4) || !strncmp(name, "mss.", 4))
   {
      const bool store = name[1] == 's';
      const int reg = store ? o.ms1 : o.md;
      if (!dtype_ok(b.Md[reg])) return fail("M register has no supported datatype programmed");
      const char *form = name + 4;
      const bool colmajor = !strcmp(form, "cm"), strided = !strcmp(form, "st") || !strcmp(form, "tst"), tst = !strcmp(form, "tst");
      if (!strcmp(form, "1r")) return fail("whole-register transfer not modelled");
      const uint64_t base = env.read_x(o.xs1), stride = strided ? env.read_x(o.xs2) : 0;
      if (!square_transfer(b, env, b.M[reg], store, colmajor, strided, tst, base, stride))
      {
         char where[64]; snprintf(where, sizeof(where), "guest memory access failed at 0x%llx (stride %llu)", (unsigned long long)base, (unsigned long long)stride);
         return fail(where);
      }
      return true;
   }

   /* matrix multiply-accumulate: C += A x B, A^T x B, A x B^T, and the negating forms */
   if (!strncmp(name, "mmul", 4) && strstr(name, ".2d"))
   {
      const uint32_t dt = b.Ad[o.ad];
      if (!dtype_ok(dt) || b.Md[o.ms1] != dt || b.Md[o.ms2] != dt) return fail("operands need one supported datatype (Int32 or FP32) in common");
      const bool at = strstr(name, "mmulat") != 0, bt = strstr(name, "mmulbt") != 0, neg = strstr(name, "neg") != 0;
      const int32_t *A = b.M[o.ms1], *B = b.M[o.ms2];
      int32_t *C = b.Acc[o.ad];
      for (unsigned i = 0; i < N; i++)
         for (unsigned j = 0; j < N; j++)
         {
            int32_t s = C[i * N + j];
            float fs = as_f(s);
            for (unsigned q = 0; q < N; q++)
            {
               const int32_t a = at ? A[q * N + i] : A[i * N + q];
               const int32_t bb = bt ? B[j * N + q] : B[q * N + j];
               if (dt == DTYPE_FP32)
                  fs = neg ? fs - as_f(a) * as_f(bb) : fs + as_f(a) * as_f(bb);   /* fixed order q = 0..N-1, seeded with the accumulator */
               else
                  s = neg ? s - a * bb : s + a * bb;
            }
            C[i * N + j] = dt == DTYPE_FP32 ? as_i(fs) : s;
         }
      return true;
   }

   /* moves and zeroing */
   if (!strcmp(name, "mmov.m.a"))
   {
      const int src = o.as1 >= 0 ? o.as1 : o.ad;    /* the accumulator is a source here */
      if (b.Md[o.md] != b.Ad[src]) return fail("datatype mismatch between M register and accumulator (amestatus.UN)");
      memcpy(b.M[o.md], b.Acc[src], sizeof(b.M[o.md])); return true;
   }
   if (!strcmp(name, "mmov.a.m"))
   {
      if (b.Md[o.ms1] != b.Ad[o.ad]) return fail("datatype mismatch between M register and accumulator (amestatus.UN)");
      memcpy(b.Acc[o.ad], b.M[o.ms1], sizeof(b.Acc[o.ad])); return true;
   }
   if (!strcmp(name, "mmov.m.m")) { b.Md[o.md] = b.Md[o.ms1]; memcpy(b.M[o.md], b.M[o.ms1], sizeof(b.M[o.md])); return true; }
   if (!strcmp(name, "mzero.2d.acc")) { memset(b.Acc[o.ad], 0, sizeof(b.Acc[o.ad])); return true; }
   if (!strcmp(name, "mzero.2d.m")) { memset(b.M[o.md], 0, sizeof(b.M[o.md])); return true; }

   /* elementwise */
   if (!strcmp(name, "madd.ew") || !strcmp(name, "msub.ew") || !strcmp(name, "mmul.ew"))
   {
      const uint32_t dt = b.Md[o.md];
      if (!dtype_ok(dt) || b.Md[o.ms1] != dt || b.Md[o.ms2] != dt) return fail("operands need one supported datatype (Int32 or FP32) in common");
      for (unsigned e = 0; e < NELEM; e++)
      {
         const int32_t x = b.M[o.ms1][e], y = b.M[o.ms2][e];
         if (dt == DTYPE_FP32)
         {
            const float fx = as_f(x), fy = as_f(y);
            b.M[o.md][e] = as_i(name[1] == 'a' ? fx + fy : name[1] == 's' ? fx - fy : fx * fy);
         }
         else
            b.M[o.md][e] = name[1] == 'a' ? x + y : name[1] == 's' ? x - y : x * y;
      }
      return true;
   }
   return fail("instruction not modelled yet");
}

}
