/* A small but ordinary workload for the configuration regression suite.
 *
 * The suite used to run /bin/true, which is a poor choice for a simulator:
 * it does no meaningful work, touches almost no memory, and exits before a
 * frontend has much to instrument.  Sniper does not handle it well, so a
 * failure there says more about the corner case than about the configuration
 * under test.
 *
 * This does what a normal program does -- allocates, walks memory, mixes integer
 * and floating-point work, calls into libc -- while still finishing quickly.
 * It prints a deterministic checksum so a run that silently produces nothing
 * can be told apart from one that worked.
 */
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

#ifndef WORK_N
#define WORK_N 4096
#endif
#ifndef WORK_ITERS
#define WORK_ITERS 24
#endif

int main(void)
{
   int *a = malloc(WORK_N * sizeof(int));
   double *b = malloc(WORK_N * sizeof(double));
   if (!a || !b) {
      fprintf(stderr, "allocation failed\n");
      return 1;
   }

   for (int i = 0; i < WORK_N; i++) {
      a[i] = i * 7 + 1;
      b[i] = (double)i / 3.0;
   }

   long isum = 0;
   double fsum = 0.0;
   for (int it = 0; it < WORK_ITERS; it++) {
      for (int i = 0; i < WORK_N; i++) {
         int j = (i * 31 + it) & (WORK_N - 1);   /* scattered reads */
         isum += a[j] ^ (long)it;
         fsum += b[j] * 1.5;
      }
      memmove(a, a + 1, (WORK_N - 1) * sizeof(int));
      a[WORK_N - 1] = (int)(isum & 0xffff);
   }

   printf("workload isum=%ld fsum=%.3f\n", isum, fsum);
   free(a);
   free(b);
   return 0;
}
