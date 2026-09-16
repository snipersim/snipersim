/* K2: the per-thread ROI barrier.
 *
 * SimRoiStart/SimRoiEnd cannot express "start measuring once all my threads
 * are ready": whichever thread reaches SimRoiStart first opens ROI, and the
 * others are measured from wherever they happen to be.  The barrier fixes
 * that -- the coordinator declares how many threads will take part, each
 * joins from its own context, and the performance models turn on when the
 * last one arrives.
 *
 * The two phases here are what make the gating observable rather than merely
 * the enabling.  Phase 1 runs a large amount of work with only one of the two
 * expected threads joined, so a barrier that opened ROI on the first join
 * would measure it.  Phase 2 is deliberately much smaller.  The simulated
 * instruction count therefore says which happened, without depending on how
 * the guest's output interleaves with the simulator's.
 */
#include <pthread.h>
#include <stdio.h>
#include <stdint.h>
#include "sim_api.h"

#define PHASE1_ITERS 4000000L   /* only thread A is joined -- must NOT be measured */
#define PHASE2_ITERS 20000L     /* both joined -- must be measured */

#define ID_A 1
#define ID_B 2

static volatile double sink;
static pthread_barrier_t phase1_done;

static double spin(long iters, long seed)
{
   double s = 0.0;
   for (long i = 1; i <= iters; i++)
      s += 1.0 / (double)(i + seed);
   return s;
}

/* Thread B joins the ROI barrier only after thread A has finished phase 1. */
static void *thread_b(void *arg)
{
   (void)arg;
   pthread_barrier_wait(&phase1_done);

   SimThreadRoiStart(ID_B);          /* second and last joiner: ROI opens here */
   sink += spin(PHASE2_ITERS, 7);
   SimThreadRoiEnd(ID_B);
   return NULL;
}

int main(void)
{
   pthread_t b;

   SimRoiExpect(2);
   printf("roi_barrier expecting 2\n");
   fflush(stdout);

   pthread_barrier_init(&phase1_done, NULL, 2);
   pthread_create(&b, NULL, thread_b, NULL);

   /* Phase 1: A is joined, B is not.  The barrier is incomplete, so none of
      this work should be measured. */
   SimThreadRoiStart(ID_A);
   sink += spin(PHASE1_ITERS, 3);
   printf("roi_barrier phase1 done (1 of 2 joined)\n");
   fflush(stdout);

   /* Phase 2: releasing B completes the barrier and opens ROI. */
   pthread_barrier_wait(&phase1_done);
   sink += spin(PHASE2_ITERS, 5);
   SimThreadRoiEnd(ID_A);

   pthread_join(b, NULL);
   pthread_barrier_destroy(&phase1_done);

   printf("roi_barrier done sink=%.6f\n", sink);
   return 0;
}
