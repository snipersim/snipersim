/* Barrier-synchronised workload with phases that behave differently.
 *
 * This exists to validate sampled simulation (Pac-Sim / --dynamic-sampling),
 * and its shape is the whole point.
 *
 * A sampled methodology simulates a fraction of the run in detail and
 * extrapolates the rest.  Against a uniform workload that is trivially
 * accurate: every interval looks like every other, so any sample -- however
 * badly chosen -- extrapolates correctly, and a test built on one would pass
 * without telling you anything about the sampler.
 *
 * So the phases here alternate between two deliberately dissimilar
 * behaviours:
 *
 *   MEMORY   a pointer chase around a randomised cycle through a buffer much
 *            larger than the last-level cache.  Each load depends on the one
 *            before it, so the phase runs at memory latency and its IPC is
 *            low.
 *
 *   COMPUTE  a dependent floating-point chain held entirely in registers, no
 *            memory traffic at all.  IPC is high and bounded by FPU latency.
 *
 * The two differ by roughly an order of magnitude in IPC, so a sampler that
 * picks its detailed intervals badly -- or that assumes phases resemble their
 * neighbours -- produces a visibly wrong total.  That is what makes the
 * accuracy check meaningful.
 *
 * Threads meet at a barrier between phases, which is also what Pac-Sim's
 * intra-barrier analysis is built around.
 *
 * Deterministic: the permutation is generated from a fixed seed with an
 * explicit LCG rather than rand(), so the checksum does not depend on the
 * platform's libc, and the same build always simulates the same instructions.
 *
 * The phase loop is marked as the region of interest, and that is not a
 * detail.  Building the chase cycles is a Fisher-Yates shuffle over every slot
 * of every thread's buffer, and measurement showed it costs more instructions
 * than all the phases put together -- 70.2M simulated, of which only ~16M were
 * phases.  Sampling that would mean spending the detailed budget on a one-off
 * region with no phase structure at all, and reporting an accuracy figure
 * dominated by it.  SimRoiStart/SimRoiEnd confine both the baseline and the
 * sampled run to the part of the program this test is actually about.
 *
 * The markers compile to `xchg %bx, %bx`, which is a no-op on real hardware,
 * so the workload still runs and checksums correctly outside the simulator.
 */
#include <pthread.h>
#include <stdint.h>
#include <stdio.h>
#include <stdlib.h>

#include "sim_api.h"

#ifndef NTHREADS
#define NTHREADS 4
#endif
#ifndef NPHASES
#define NPHASES 16
#endif
/* Per-thread chase buffer, in 8-byte slots.  2^19 slots is 4 MiB per thread,
 * comfortably past a typical 8 MiB shared LLC once four threads are running. */
#ifndef CHASE_SLOTS
#define CHASE_SLOTS (1 << 19)
#endif
#ifndef CHASE_STEPS
#define CHASE_STEPS 200000
#endif
#ifndef COMPUTE_ITERS
#define COMPUTE_ITERS 400000
#endif

static pthread_barrier_t barrier;
static size_t *chase[NTHREADS];
static double  result[NTHREADS];
static uint64_t visited[NTHREADS];

/* Deterministic LCG -- the Numerical Recipes constants. */
static uint32_t lcg(uint32_t *s)
{
   *s = (*s * 1664525u) + 1013904223u;
   return *s;
}

/* Build a single cycle visiting every slot exactly once, so the chase cannot
 * fall into a short loop that fits in cache. */
static void build_cycle(size_t *a, size_t n, uint32_t seed)
{
   size_t *order = malloc(n * sizeof(size_t));
   uint32_t s = seed;

   for (size_t i = 0; i < n; i++)
      order[i] = i;
   for (size_t i = n - 1; i > 0; i--)
   {
      size_t j = lcg(&s) % (i + 1);
      size_t t = order[i]; order[i] = order[j]; order[j] = t;
   }
   for (size_t i = 0; i < n; i++)
      a[order[i]] = order[(i + 1) % n];

   free(order);
}

static void *work(void *arg)
{
   const long id = (long)arg;
   size_t *a = chase[id];
   double acc = 0.0;
   uint64_t seen = 0;
   size_t p = 0;

   for (int phase = 0; phase < NPHASES; phase++)
   {
      pthread_barrier_wait(&barrier);

      if ((phase & 1) == 0)
      {
         /* MEMORY: dependent loads, one cache miss at a time. */
         for (long i = 0; i < CHASE_STEPS; i++)
            p = a[p];
         seen += p;
      }
      else
      {
         /* COMPUTE: dependent FP chain, no memory traffic. */
         double x = 1.0 + (double)id;
         for (long i = 0; i < COMPUTE_ITERS; i++)
            x = x * 0.9999999 + 1e-7;
         acc += x;
      }
   }

   result[id]  = acc;
   visited[id] = seen;
   return NULL;
}

int main(void)
{
   pthread_t th[NTHREADS];
   double total = 0.0;
   uint64_t seen = 0;

   pthread_barrier_init(&barrier, NULL, NTHREADS);
   for (long i = 0; i < NTHREADS; i++)
   {
      chase[i] = malloc(CHASE_SLOTS * sizeof(size_t));
      if (!chase[i])
      {
         fprintf(stderr, "phased: out of memory\n");
         return 1;
      }
      build_cycle(chase[i], CHASE_SLOTS, 12345u + (uint32_t)i);
   }

   /* Thread creation is inside the region: the barriers between phases are
    * part of what is being measured. */
   SimRoiStart();
   for (long i = 0; i < NTHREADS; i++)
      pthread_create(&th[i], NULL, work, (void *)i);
   for (int i = 0; i < NTHREADS; i++)
      pthread_join(th[i], NULL);
   SimRoiEnd();

   for (int i = 0; i < NTHREADS; i++)
   {
      total += result[i];
      seen  += visited[i];
   }

   pthread_barrier_destroy(&barrier);
   for (int i = 0; i < NTHREADS; i++)
      free(chase[i]);

   /* Printed so the run cannot be optimised away and so both the sampled and
    * the full run can be shown to have executed the same program. */
   printf("phased: threads=%d phases=%d checksum=%.6f visited=%llu\n",
          NTHREADS, NPHASES, total, (unsigned long long)seen);
   return 0;
}
