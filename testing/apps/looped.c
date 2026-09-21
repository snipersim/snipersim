/* Long-running loops with no barriers: the shape LoopPoint-style sampling is for.
 *
 * Pac-Sim's region records are typed (sift/recorder/mtng.h, RecordType).
 * MultiThread and SingleThread records describe regions delimited by global
 * barriers; IntraBarrier records describe regions found *inside* a barrier
 * interval, which is what the analysis scripts count as looppoints
 * (markerdistribution() in pacsimscripts/mtng_dumpjson.py).
 *
 * testing/apps/phased.c produces the first kind: it synchronises on a barrier
 * between every phase, so there is always an obvious place to cut.  This file
 * produces the second, and does it by taking that crutch away:
 *
 *   - threads are synchronised exactly twice, on entry and exit, and never in
 *     between.  There is no barrier inside the measured region at all.
 *   - each thread runs one long outer loop, so the only structure available to
 *     hang a region boundary on is the loop's own back edge.
 *   - behaviour changes *within* that loop, on a period that is deliberately
 *     not a divisor of anything the sampler can see, so the interesting
 *     boundaries fall at loop iterations rather than at synchronisation.
 *   - the threads are given different amounts of work, so they drift out of
 *     step and no global cut point exists even in principle.
 *
 * A sampler that can only cut at barriers has, in this workload, exactly two
 * places to cut and no way to see the structure in between.  That is the point.
 *
 * Determinism: per-thread results are stored by index and summed in a fixed
 * order at the end, so the checksum does not depend on how the threads
 * interleave.
 */
#include <pthread.h>
#include <stdint.h>
#include <stdio.h>
#include <stdlib.h>

#include "sim_api.h"

#ifndef NTHREADS
#define NTHREADS 4
#endif
/* Outer loop iterations per thread.  Each one is a candidate region boundary. */
#ifndef NITERS
#define NITERS 24
#endif
#ifndef CHASE_SLOTS
#define CHASE_SLOTS (1 << 19)
#endif
#ifndef CHASE_STEPS
#define CHASE_STEPS 120000
#endif
#ifndef COMPUTE_ITERS
#define COMPUTE_ITERS 240000
#endif

static pthread_barrier_t gate;          /* entry and exit only */
static size_t  *chase[NTHREADS];
static double   result[NTHREADS];
static uint64_t visited[NTHREADS];

/* Build a single cycle visiting every slot exactly once.
 *
 * A full-period LCG over 0..n-1 is a permutation that is one n-cycle, so
 * a[x] = (A*x + C) mod n is the cycle, written in a single pass with no
 * scratch allocation.  Hull-Dobell gives full period for a power-of-two
 * modulus when C is odd and A = 1 (mod 4).  See the note in phased.c: the
 * Fisher-Yates this replaces ran before the region of interest and dominated
 * the fixed cost of a run.
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

   pthread_barrier_wait(&gate);   /* every worker exists */
   pthread_barrier_wait(&gate);   /* main has opened the region */

   /* Threads get different iteration counts, so they finish their phases at
    * different times and never line up.  A global barrier could not be placed
    * here even if the workload wanted one. */
   const long iters = NITERS + id;

   for (long it = 0; it < iters; it++)
   {
      /* The behaviour changes on a period of three, which does not divide the
       * thread count, the iteration count, or any sampling interval that is a
       * power of ten.  The change is visible only from inside the loop. */
      if (it % 3 == 0)
      {
         for (long i = 0; i < CHASE_STEPS; i++)
            p = a[p];
         seen += p;
      }
      else
      {
         double x = 1.0 + (double)(id + it);
         for (long i = 0; i < COMPUTE_ITERS; i++)
            x = x * 0.9999999 + 1e-7;
         acc += x;
      }
   }

   result[id]  = acc;
   visited[id] = seen;

   pthread_barrier_wait(&gate);   /* all iterations finished */
   return NULL;
}

int main(void)
{
   pthread_t th[NTHREADS];
   double total = 0.0;
   uint64_t seen = 0;

   pthread_barrier_init(&gate, NULL, NTHREADS + 1);
   for (long i = 0; i < NTHREADS; i++)
   {
      chase[i] = malloc(CHASE_SLOTS * sizeof(size_t));
      if (!chase[i])
      {
         fprintf(stderr, "looped: out of memory\n");
         return 1;
      }
      build_cycle(chase[i], CHASE_SLOTS, 4321u + (uint32_t)i);
   }

   /* Thread creation and join stay outside the region: measured at 205.7M
    * cycles of otherwise empty run, which would be most of the answer. */
   for (long i = 0; i < NTHREADS; i++)
      pthread_create(&th[i], NULL, work, (void *)i);

   pthread_barrier_wait(&gate);
   SimRoiStart();
   pthread_barrier_wait(&gate);

   pthread_barrier_wait(&gate);

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
   printf("looped: threads=%d iters=%d checksum=%.6f visited=%llu\n",
          NTHREADS, NITERS, total, (unsigned long long)seen);
   fflush(stdout);
   SimRoiEnd();

   for (int i = 0; i < NTHREADS; i++)
      pthread_join(th[i], NULL);

   pthread_barrier_destroy(&gate);
   for (int i = 0; i < NTHREADS; i++)
      free(chase[i]);
   return 0;
}
