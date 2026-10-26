/* phased.c for a RISC-V or AArch64 guest without a C library: the barrier-synchronised
 * phased workload, multi-threaded, freestanding.  Threads are cloned with
 * the raw system call onto static stacks, the barrier is a sense-reversing
 * one on a futex, the join waits on each child's tid word (the kernel
 * clears it and wakes the waiter: CLONE_CHILD_CLEARTID), output goes
 * through write, and the checksum is an integer.  The shape is phased.c's:
 * NTHREADS workers alternate a pointer chase with arithmetic, meeting at a
 * barrier between phases; main opens the region once every worker is up,
 * prints the checksum inside it, and closes it before joining.
 *
 * The barrier routine is what the sampler keys its synchronisation points
 * on, by its symbol: pass pacsim-sync=phased_barrier_wait to the plugin.
 *
 * Build:
 *   $SNIPER_RISCV_CC --target=riscv64-linux-gnu -march=rv64gc -mabi=lp64d -O1 \
 *       -ffreestanding -nostdlib -static -fuse-ld=lld -Wl,-e,_start -I include \
 *       -DNTHREADS=4 -o phased_rv_mt testing/apps/phased_rv_mt.c
 * and for AArch64 the same with --target=aarch64-linux-gnu (no -march/-mabi):
 * both use the asm-generic system-call numbers; only the trap and the clone
 * stub differ. */
#include <stdint.h>
#include "sim_api.h"

#ifndef NTHREADS
#define NTHREADS 4
#endif
#ifndef NPHASES
#define NPHASES 12
#endif
#ifndef CHASE_SLOTS
#define CHASE_SLOTS (1 << 17)
#endif
#ifndef CHASE_STEPS
#define CHASE_STEPS 60000
#endif
#ifndef COMPUTE_ITERS
#define COMPUTE_ITERS 120000
#endif

#define STACK_BYTES (256 * 1024)

/* system calls: the generic (asm-generic) numbers */
enum { SYS_write_ = 64, SYS_exit_ = 93, SYS_exit_group_ = 94, SYS_futex_ = 98, SYS_clone_ = 220 };
enum { FUTEX_WAIT_ = 0, FUTEX_WAKE_ = 1 };

static long sys6(long n, long a, long b, long c, long d, long e, long f)
{
#if defined(__aarch64__)
   register long a0 __asm__("x0") = a; register long a1 __asm__("x1") = b; register long a2 __asm__("x2") = c;
   register long a3 __asm__("x3") = d; register long a4 __asm__("x4") = e; register long a5 __asm__("x5") = f;
   register long a7 __asm__("x8") = n;
   __asm__ volatile("svc #0" : "+r"(a0) : "r"(a1), "r"(a2), "r"(a3), "r"(a4), "r"(a5), "r"(a7) : "memory");
#else
   register long a0 __asm__("a0") = a; register long a1 __asm__("a1") = b; register long a2 __asm__("a2") = c;
   register long a3 __asm__("a3") = d; register long a4 __asm__("a4") = e; register long a5 __asm__("a5") = f;
   register long a7 __asm__("a7") = n;
   __asm__ volatile("ecall" : "+r"(a0) : "r"(a1), "r"(a2), "r"(a3), "r"(a4), "r"(a5), "r"(a7) : "memory");
#endif
   return a0;
}
static void out(const char *s) { long n = 0; while (s[n]) n++; sys6(SYS_write_, 1, (long)s, n, 0, 0, 0); }
static void outnum(unsigned long v) { char b[24]; int i = 23; b[i] = 0; do { b[--i] = '0' + v % 10; v /= 10; } while (v); out(b + i); }

/* clone(flags, stack, parent_tid, tls, child_tid): the child starts on its
 * stack with the function and argument stored there, calls, and exits. */
long rv_clone(void (*fn)(long), void *stack_top, unsigned long flags, long arg, int *child_tid);
#if defined(__aarch64__)
__asm__(
   ".text\n"
   ".globl rv_clone\n"
   ".type rv_clone, %function\n"
   "rv_clone:\n"
   "   sub  x1, x1, #16\n"        /* the child's frame: fn, arg */
   "   stp  x0, x3, [x1]\n"
   "   mov  x0, x2\n"             /* flags */
   "   mov  x2, xzr\n"            /* parent_tid */
   "   mov  x3, xzr\n"            /* tls; x4 = child_tid already */
   "   mov  x8, #220\n"           /* clone */
   "   svc  #0\n"
   "   cbnz x0, 1f\n"             /* the parent (or an error): return the tid */
   "   ldp  x1, x0, [sp]\n"       /* the child: call fn(arg), then exit */
   "   blr  x1\n"
   "   mov  x8, #93\n"
   "   svc  #0\n"
   "1: ret\n"
   ".size rv_clone, .-rv_clone\n");
#else
__asm__(
   ".text\n"
   ".globl rv_clone\n"
   ".type rv_clone, @function\n"
   "rv_clone:\n"
   "   addi a1, a1, -16\n"        /* the child's frame: fn, arg */
   "   sd   a0, 0(a1)\n"
   "   sd   a3, 8(a1)\n"
   "   mv   a0, a2\n"             /* flags */
   "   mv   a2, zero\n"           /* parent_tid */
   "   mv   a3, zero\n"           /* tls */
   "   li   a7, 220\n"            /* clone; a4 = child_tid already */
   "   ecall\n"
   "   bnez a0, 1f\n"             /* the parent (or an error): return the tid */
   "   ld   a1, 0(sp)\n"          /* the child: call fn(arg), then exit */
   "   ld   a0, 8(sp)\n"
   "   jalr a1\n"
   "   li   a7, 93\n"
   "   ecall\n"
   "1: ret\n"
   ".size rv_clone, .-rv_clone\n");
#endif

/* CLONE_VM | CLONE_FS | CLONE_FILES | CLONE_SIGHAND | CLONE_THREAD | CLONE_SYSVSEM
 * | CLONE_CHILD_CLEARTID | CLONE_CHILD_SETTID: what pthread_create asks for, less TLS */
#define CLONE_FLAGS 0x1250f00ul

/* A sense-reversing barrier: the last to arrive flips the sense and wakes
 * the rest, who sleep on the futex until it flips.  Not inlined: the
 * sampler recognises it by its symbol, and keys each barrier by the call
 * site it returns to. */
struct barrier { int count; int sense; int n; };
__attribute__((noinline)) void phased_barrier_wait(struct barrier *b)
{
   const int s = 1 - __atomic_load_n(&b->sense, __ATOMIC_ACQUIRE);
   if (__atomic_add_fetch(&b->count, 1, __ATOMIC_ACQ_REL) == b->n)
   {
      __atomic_store_n(&b->count, 0, __ATOMIC_RELAXED);
      __atomic_store_n(&b->sense, s, __ATOMIC_RELEASE);
      sys6(SYS_futex_, (long)&b->sense, FUTEX_WAKE_, 0x7fffffff, 0, 0, 0);
   }
   else
      while (__atomic_load_n(&b->sense, __ATOMIC_ACQUIRE) != s)
         sys6(SYS_futex_, (long)&b->sense, FUTEX_WAIT_, 1 - s, 0, 0, 0);
}

static struct barrier barrier = { 0, 0, NTHREADS };       /* between phases: the workers */
static struct barrier gate    = { 0, 0, NTHREADS + 1 };   /* main + workers: brackets the ROI */
static uint32_t chase[NTHREADS][CHASE_SLOTS];
static uint64_t result[NTHREADS];
static uint64_t visited[NTHREADS];
static int      tids[NTHREADS];
static uint8_t  stacks[NTHREADS][STACK_BYTES] __attribute__((aligned(16)));

/* One cycle through every slot: a full-period LCG over 0..n-1 (phased.c). */
static void build_cycle(uint32_t *a, uint32_t n, uint32_t seed)
{
   const uint64_t A = 1664525u, C = 1013904223u;
   const uint32_t mask = n - 1;
   uint32_t x = seed & mask;
   for (uint32_t i = 0; i < n; i++)
   {
      const uint32_t next = (uint32_t)((A * (uint64_t)x + C) & mask);
      a[x] = next;
      x = next;
   }
}

static void work(long t)
{
   uint32_t *a = chase[t];
   uint64_t acc = 0x9e3779b97f4a7c15ull ^ (uint64_t)t;
   uint64_t seen = 0;
   uint32_t p = (uint32_t)t;

   /* every thread exists, then main opened the region */
   phased_barrier_wait(&gate);
   phased_barrier_wait(&gate);

   for (int phase = 0; phase < NPHASES; phase++)
   {
      if (phase & 1)
         for (long i = 0; i < COMPUTE_ITERS; i++) acc = acc * 6364136223846793005ull + (uint64_t)(i & 7);
      else
         for (long i = 0; i < CHASE_STEPS; i++) { p = a[p]; seen++; }
      phased_barrier_wait(&barrier);
   }
   result[t] = acc + p;
   visited[t] = seen;
   phased_barrier_wait(&gate);      /* phases done; main sums and prints */
}

void _start(void)
{
   for (long t = 0; t < NTHREADS; t++)
      build_cycle(chase[t], CHASE_SLOTS, 12345u + 7919u * (uint32_t)t);

   for (long t = 0; t < NTHREADS; t++)
   {
      tids[t] = 1;                  /* nonzero until the child has run and exited */
      if (rv_clone(work, stacks[t] + STACK_BYTES, CLONE_FLAGS, t, &tids[t]) < 0)
      {
         out("phased_rv_mt: clone failed\n");
         sys6(SYS_exit_group_, 1, 0, 0, 0, 0, 0);
      }
   }

   phased_barrier_wait(&gate);      /* every worker is up */
   SimRoiStart();
   phased_barrier_wait(&gate);      /* let them run */
   phased_barrier_wait(&gate);      /* every worker has finished its phases */

   uint64_t total = 0, seen = 0;
   for (int t = 0; t < NTHREADS; t++) { total += result[t]; seen += visited[t]; }
   out("phased_rv_mt: threads="); outnum(NTHREADS); out(" phases="); outnum(NPHASES);
   out(" checksum="); outnum(total); out(" visited="); outnum(seen); out("\n");
   SimRoiEnd();

   /* join: the kernel clears each tid word at the child's exit and wakes us */
   for (int t = 0; t < NTHREADS; t++)
   {
      int v;
      while ((v = __atomic_load_n(&tids[t], __ATOMIC_ACQUIRE)) != 0)
         sys6(SYS_futex_, (long)&tids[t], FUTEX_WAIT_, v, 0, 0, 0);
   }
   sys6(SYS_exit_group_, 0, 0, 0, 0, 0, 0);
   for (;;) {}
}
