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

static pthread_barrier_t barrier;   /* between phases: the workers       */
static pthread_barrier_t gate;      /* main + workers: brackets the ROI   */
static size_t *chase[NTHREADS];
static double  result[NTHREADS];
static uint64_t visited[NTHREADS];

/* Build a single cycle visiting every slot exactly once.
 *
 * A full-period LCG over 0..n-1 is a permutation that is one n-cycle, so
 * a[x] = (A*x + C) mod n is the cycle, written in a single pass with no
 * scratch allocation.  Hull-Dobell gives full period for a power-of-two
 * modulus when C is odd and A = 1 (mod 4), which is why n must be a power of
 * two and why these constants are the ones used.
 *
 * This replaces a Fisher-Yates shuffle, which needed three passes and an
 * n-sized scratch array.  That mattered: the shuffle runs before the region of
 * interest, and at 2^19 slots per thread it was 151s of the 260s a four-phase
 * run took -- pure fixed cost, paid identically by a sampled run and a full
 * one, so it diluted any speedup measured between them.
 *
 * Successive slots land far apart, so the chase still defeats prefetching; the
 * seed shifts where in the cycle a given slot points, keeping per-thread
 * buffers distinct.
 */
static void build_cycle(size_t *a, size_t n, uint32_t seed)
{
   const uint64_t A = 1664525u, C = 1013904223u;
   const size_t mask = n - 1;        /* n is a power of two */
   size_t x = seed & mask;

   for (size_t i = 0; i < n; i++)
   {
      size_t next = (size_t)((A * (uint64_t)x + C) & mask);
      a[x] = next;
      x = next;
   }
}

static void *work(void *arg)
{
   const long id = (long)arg;
   size_t *a = chase[id];
   double acc = 0.0;
   uint64_t seen = 0;
   size_t p = 0;

   /* Wait for every thread to exist, then for main to open the region. */
   pthread_barrier_wait(&gate);
   pthread_barrier_wait(&gate);

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

   /* Phases done; main sums, prints and closes the region before joining. */
   pthread_barrier_wait(&gate);
   return NULL;
}

int main(void)
{
   pthread_t th[NTHREADS];
   double total = 0.0;
   uint64_t seen = 0;

   pthread_barrier_init(&barrier, NULL, NTHREADS);
   pthread_barrier_init(&gate, NULL, NTHREADS + 1);
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

   /* Thread creation and join are kept OUT of the region.
    *
    * Measured: with them inside, an otherwise empty run -- every phase body
    * compiled away -- still cost 205.7M cycles at zero instructions, which was
    * 69% of the four-phase run.  Spawning and reaping threads is modelled as a
    * large, one-off stretch of elapsed time with no phase structure in it, so
    * leaving it in would hand a sampler most of the answer for free and let a
    * lazy one score well.  The gate barrier lets main open the region only once
    * every worker exists, and close it once they have all finished their
    * phases. */
   for (long i = 0; i < NTHREADS; i++)
      pthread_create(&th[i], NULL, work, (void *)i);

   pthread_barrier_wait(&gate);      /* every worker is up */
   SimRoiStart();
   pthread_barrier_wait(&gate);      /* let them run */

   pthread_barrier_wait(&gate);      /* every worker has finished its phases */

   for (int i = 0; i < NTHREADS; i++)
   {
      total += result[i];
      seen  += visited[i];
   }
   /* The results are stored before the last gate, and main sums and prints
    * them after it, still inside the ROI: whatever happens to the process
    * once the region is closed and the workers exit -- the recorder has died
    * there, inside SDE, with the workers leaving while main went on -- the
    * checksum is already out, and the sampled run can be compared with the
    * full one. */
   printf("phased: threads=%d phases=%d checksum=%.6f visited=%llu\n",
          NTHREADS, NPHASES, total, (unsigned long long)seen);
   fflush(stdout);
   SimRoiEnd();

   for (int i = 0; i < NTHREADS; i++)
      pthread_join(th[i], NULL);

   pthread_barrier_destroy(&barrier);
   pthread_barrier_destroy(&gate);
   for (int i = 0; i < NTHREADS; i++)
      free(chase[i]);
   return 0;
}
