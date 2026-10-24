/* One thread, phases alternating a pointer chase with arithmetic, a loop
 * back-edge per phase: the single-threaded shape for a front end that can
 * only run one thread on a live channel (the QEMU plugin in user mode until
 * it can wait for Sniper outside QEMU's run loop).  Same conventions as
 * phased.c: an explicit LCG, SimRoiStart/SimRoiEnd around the phases, the
 * checksum printed inside the ROI. */
#include <stdint.h>
#include <stdio.h>
#include <stdlib.h>
#include "sim_api.h"

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

static uint64_t lcg(uint64_t *s) { *s = *s * 6364136223846793005ull + 1442695040888963407ull; return *s >> 33; }

int main(void)
{
   uint32_t *chase = malloc(CHASE_SLOTS * sizeof(uint32_t));
   if (!chase) { fprintf(stderr, "phased_st: out of memory\n"); return 1; }
   uint64_t seed = 12345;
   for (uint32_t i = 0; i < CHASE_SLOTS; i++) chase[i] = i;
   for (uint32_t i = CHASE_SLOTS - 1; i > 0; i--) { uint32_t j = (uint32_t)(lcg(&seed) % (i + 1)); uint32_t t = chase[i]; chase[i] = chase[j]; chase[j] = t; }
   double acc = 0; unsigned long long seen = 0; uint32_t p = 0;

   SimRoiStart();
   for (int phase = 0; phase < NPHASES; phase++)
   {
      if (phase & 1)
         for (long i = 0; i < COMPUTE_ITERS; i++) acc = acc * 0.999999 + (double)(i & 7) * 1e-6;
      else
         for (long i = 0; i < CHASE_STEPS; i++) { p = chase[p]; seen++; }
   }
   printf("phased_st: phases=%d checksum=%.6f visited=%llu p=%u\n", NPHASES, acc, seen, (unsigned)p);
   fflush(stdout);
   SimRoiEnd();
   free(chase);
   return 0;
}
