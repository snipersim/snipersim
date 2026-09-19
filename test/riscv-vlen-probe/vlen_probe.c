/* A probe of the vector unit at its full width: every instruction family the
 * agent's kernels use, on a whole m8 (or e8 m2) register group at the VLEN the
 * CPU runs, against a scalar reference.  Freestanding; the exit code says what
 * failed: 100 when everything agrees, otherwise 10 * family + step.
 *
 *   1 e8 load/store (m2)        2 e32 load/store (m8)      3 vand.vi / vsrl.vi (e8)
 *   4 vzext.vf4 e8 -> e32       5 vadd.vi (e32 m8)         6 vadd.vv / vmul.vv (e32 m8)
 *   7 vfcvt.f.x.v               8 vfmacc.vv                9 vfredusum.vs (m8)
 *  10 vmv.v.i / vmv.v.x        11 vsll.vi / vsrl.vi (e32)  12 vluxei32.v (indexed, m8)
 *  13 vslideup / vrgather       14 vfredusum over a padded (tail-undisturbed) group
 *
 * QEMU's generic-vector expansion (families 5, 6, 10, 11) was found wrong above
 * 2 KB operands when its VLEN cap was raised: this is the test that pins it
 * (testing/test_qemu.py riscv-vlen-probe runs it at every VLEN). */
#include <stdint.h>

static void sys_exit(long code) { register long a0 __asm__("a0") = code; register long a7 __asm__("a7") = 93; __asm__ volatile("ecall" : : "r"(a0), "r"(a7)); for (;;) ; }

#define MAXE 16384                         /* e32 m8 at VLEN 65536 */
static uint8_t  b_in[MAXE] __attribute__((aligned(64))), b_out[MAXE] __attribute__((aligned(64)));
static uint32_t w_in[MAXE] __attribute__((aligned(64))), w_out[MAXE] __attribute__((aligned(64))), w_idx[MAXE] __attribute__((aligned(64)));
static float    f_in[MAXE] __attribute__((aligned(64))), f_out[MAXE] __attribute__((aligned(64))), f_x[MAXE] __attribute__((aligned(64)));

static long vlenb(void) { long v; __asm__ volatile("csrr %0, vlenb" : "=r"(v)); return v; }
static float fabs_(float x) { return x < 0 ? -x : x; }

int main_probe(void)
{
   const long VB = vlenb();
   const long N8 = VB * 2;                 /* e8 m2 lanes  */
   const long N32 = VB * 8 / 4;            /* e32 m8 lanes */
   if (N32 > MAXE) return 1;
   for (long i = 0; i < N32; i++)
   {
      b_in[i] = (uint8_t)((i * 7 + 3) & 0xff);
      w_in[i] = (uint32_t)(i * 2654435761u);
      f_in[i] = (float)((i % 97) - 48) * 0.125f;
      f_x[i] = 1.0f + (float)(i % 13) * 0.25f;
      w_idx[i] = (uint32_t)(((i * 37) % N32) * 4);       /* byte offsets, a permutation of the lanes */
   }
   long vl;
   /* 1: e8 load/store, m2, full group */
   __asm__ volatile("vsetvli %0, %1, e8, m2, ta, ma\n\tvle8.v v0, (%2)\n\tvse8.v v0, (%3)" : "=&r"(vl) : "r"(N8), "r"(b_in), "r"(b_out) : "memory");
   if (vl != N8) return 11;
   for (long i = 0; i < N8; i++) if (b_out[i] != b_in[i]) return 12;
   /* 2: e32 load/store, m8, full group */
   __asm__ volatile("vsetvli %0, %1, e32, m8, ta, ma\n\tvle32.v v8, (%2)\n\tvse32.v v8, (%3)" : "=&r"(vl) : "r"(N32), "r"(w_in), "r"(w_out) : "memory");
   if (vl != N32) return 21;
   for (long i = 0; i < N32; i++) if (w_out[i] != w_in[i]) return 22;
   /* 3: vand.vi / vsrl.vi on e8 */
   __asm__ volatile("vsetvli %0, %1, e8, m2, ta, ma\n\tvle8.v v0, (%2)\n\tvand.vi v2, v0, 15\n\tvse8.v v2, (%3)" : "=&r"(vl) : "r"(N8), "r"(b_in), "r"(b_out) : "memory");
   for (long i = 0; i < N8; i++) if (b_out[i] != (b_in[i] & 15)) return 31;
   __asm__ volatile("vsetvli %0, %1, e8, m2, ta, ma\n\tvle8.v v0, (%2)\n\tvsrl.vi v2, v0, 4\n\tvse8.v v2, (%3)" : "=&r"(vl) : "r"(N8), "r"(b_in), "r"(b_out) : "memory");
   for (long i = 0; i < N8; i++) if (b_out[i] != (b_in[i] >> 4)) return 32;
   /* 4: vzext.vf4 e8 (m2) -> e32 (m8) */
   __asm__ volatile("vsetvli %0, %1, e8, m2, ta, ma\n\tvle8.v v0, (%2)\n\tvsetvli %0, %1, e32, m8, ta, ma\n\tvzext.vf4 v8, v0\n\tvse32.v v8, (%3)" : "=&r"(vl) : "r"(N32), "r"(b_in), "r"(w_out) : "memory");
   for (long i = 0; i < N32; i++) if (w_out[i] != b_in[i]) return 41;
   /* 5: vadd.vi on e32 m8 (generic-vector expansion) */
   __asm__ volatile("vsetvli %0, %1, e32, m8, ta, ma\n\tvle32.v v8, (%2)\n\tvadd.vi v8, v8, -8\n\tvse32.v v8, (%3)" : "=&r"(vl) : "r"(N32), "r"(w_in), "r"(w_out) : "memory");
   for (long i = 0; i < N32; i++) if (w_out[i] != w_in[i] - 8u) return 51;
   /* 6: vadd.vv / vmul.vv e32 m8 */
   __asm__ volatile("vsetvli %0, %1, e32, m8, ta, ma\n\tvle32.v v8, (%2)\n\tvle32.v v16, (%3)\n\tvadd.vv v24, v8, v16\n\tvse32.v v24, (%4)" : "=&r"(vl) : "r"(N32), "r"(w_in), "r"(w_idx), "r"(w_out) : "memory");
   for (long i = 0; i < N32; i++) if (w_out[i] != w_in[i] + w_idx[i]) return 61;
   __asm__ volatile("vsetvli %0, %1, e32, m8, ta, ma\n\tvle32.v v8, (%2)\n\tvle32.v v16, (%3)\n\tvmul.vv v24, v8, v16\n\tvse32.v v24, (%4)" : "=&r"(vl) : "r"(N32), "r"(w_in), "r"(w_idx), "r"(w_out) : "memory");
   for (long i = 0; i < N32; i++) if (w_out[i] != w_in[i] * w_idx[i]) return 62;
   /* 7: vfcvt.f.x.v e32 m8 */
   __asm__ volatile("vsetvli %0, %1, e32, m8, ta, ma\n\tvle32.v v8, (%2)\n\tvfcvt.f.x.v v8, v8\n\tvse32.v v8, (%3)" : "=&r"(vl) : "r"(N32), "r"(w_idx), "r"(f_out) : "memory");
   for (long i = 0; i < N32; i++) if (f_out[i] != (float)(int32_t)w_idx[i]) return 71;
   /* 8: vfmacc.vv e32 m8 */
   __asm__ volatile("vsetvli %0, %1, e32, m8, ta, ma\n\tvle32.v v8, (%2)\n\tvle32.v v16, (%3)\n\tvle32.v v24, (%2)\n\tvfmacc.vv v24, v8, v16\n\tvse32.v v24, (%4)" : "=&r"(vl) : "r"(N32), "r"(f_in), "r"(f_x), "r"(f_out) : "memory");
   for (long i = 0; i < N32; i++) { float r = f_in[i] + f_in[i] * f_x[i]; if (fabs_(f_out[i] - r) > 1e-5f * (fabs_(r) + 1.0f)) return 81; }
   /* 9: vfredusum.vs over the m8 group */
   {
      float s, ref = 0.0f;
      for (long i = 0; i < N32; i++) ref += f_in[i] * f_x[i];
      __asm__ volatile("vsetvli %0, %1, e32, m8, ta, ma\n\tvle32.v v8, (%2)\n\tvle32.v v16, (%3)\n\tvfmul.vv v8, v8, v16\n\tvmv.s.x v0, zero\n\tvfredusum.vs v0, v8, v0\n\tvfmv.f.s %4, v0" : "=&r"(vl) : "r"(N32), "r"(f_in), "r"(f_x), "f"(s) : "memory");
      __asm__ volatile("vfmv.f.s %0, v0" : "=f"(s));
      if (fabs_(s - ref) > 1e-3f * (fabs_(ref) + 1.0f)) return 91;
   }
   /* 10: vmv.v.i / vmv.v.x e32 m8 (generic-vector dup) */
   __asm__ volatile("vsetvli %0, %1, e32, m8, ta, ma\n\tvmv.v.i v8, -3\n\tvse32.v v8, (%2)" : "=&r"(vl) : "r"(N32), "r"(w_out) : "memory");
   for (long i = 0; i < N32; i++) if (w_out[i] != (uint32_t)-3) return 101;
   __asm__ volatile("vsetvli %0, %1, e32, m8, ta, ma\n\tvmv.v.x v8, %2\n\tvse32.v v8, (%3)" : "=&r"(vl) : "r"(N32), "r"(0x12345678L), "r"(w_out) : "memory");
   for (long i = 0; i < N32; i++) if (w_out[i] != 0x12345678u) return 102;
   /* 11: vsll.vi / vsrl.vi e32 m8 */
   __asm__ volatile("vsetvli %0, %1, e32, m8, ta, ma\n\tvle32.v v8, (%2)\n\tvsll.vi v8, v8, 2\n\tvse32.v v8, (%3)" : "=&r"(vl) : "r"(N32), "r"(w_in), "r"(w_out) : "memory");
   for (long i = 0; i < N32; i++) if (w_out[i] != (w_in[i] << 2)) return 111;
   __asm__ volatile("vsetvli %0, %1, e32, m8, ta, ma\n\tvle32.v v8, (%2)\n\tvsrl.vi v8, v8, 5\n\tvse32.v v8, (%3)" : "=&r"(vl) : "r"(N32), "r"(w_in), "r"(w_out) : "memory");
   for (long i = 0; i < N32; i++) if (w_out[i] != (w_in[i] >> 5)) return 112;
   /* 12: vluxei32.v indexed load, m8: a permutation of the lanes */
   __asm__ volatile("vsetvli %0, %1, e32, m8, ta, ma\n\tvle32.v v8, (%2)\n\tvluxei32.v v16, (%3), v8\n\tvse32.v v16, (%4)" : "=&r"(vl) : "r"(N32), "r"(w_idx), "r"(w_in), "r"(w_out) : "memory");
   for (long i = 0; i < N32; i++) if (w_out[i] != w_in[w_idx[i] / 4]) return 121;
   /* 13: vslideup.vi and vrgather.vv (m8) */
   __asm__ volatile("vsetvli %0, %1, e32, m8, ta, ma\n\tvle32.v v8, (%2)\n\tvmv.v.i v16, 0\n\tvslideup.vi v16, v8, 3\n\tvse32.v v16, (%3)" : "=&r"(vl) : "r"(N32), "r"(w_in), "r"(w_out) : "memory");
   for (long i = 3; i < N32; i++) if (w_out[i] != w_in[i - 3]) return 131;
   for (long i = 0; i < N32; i++) w_idx[i] = (uint32_t)((i * 37) % N32);
   __asm__ volatile("vsetvli %0, %1, e32, m8, ta, ma\n\tvle32.v v8, (%2)\n\tvle32.v v16, (%3)\n\tvrgather.vv v24, v8, v16\n\tvse32.v v24, (%4)" : "=&r"(vl) : "r"(N32), "r"(w_in), "r"(w_idx), "r"(w_out) : "memory");
   for (long i = 0; i < N32; i++) if (w_out[i] != w_in[w_idx[i]]) return 132;
   /* 14: a half-filled group with tail-undisturbed accumulation, reduced over the whole group */
   {
      float s, ref = 0.0f;
      const long half = N32 / 2;
      for (long i = 0; i < half; i++) ref += f_in[i] * f_x[i];
      __asm__ volatile("vsetvli %0, zero, e32, m8, ta, ma\n\tvmv.v.i v24, 0\n\tvsetvli %0, %1, e32, m8, tu, ma\n\tvle32.v v8, (%2)\n\tvle32.v v16, (%3)\n\tvfmacc.vv v24, v8, v16\n\tvsetvli %0, zero, e32, m8, ta, ma\n\tvmv.s.x v0, zero\n\tvfredusum.vs v0, v24, v0" : "=&r"(vl) : "r"(half), "r"(f_in), "r"(f_x) : "memory");
      __asm__ volatile("vfmv.f.s %0, v0" : "=f"(s));
      if (fabs_(s - ref) > 1e-3f * (fabs_(ref) + 1.0f)) return 141;
   }
   return 0;
}

void _start(void)
{
   const int r = main_probe();
   sys_exit(r == 0 ? 100 : r);
}
