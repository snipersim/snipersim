/* Regression workload for the embedded Python scripting hooks.
 *
 * Spawns several worker threads so that simulator hook callbacks -- and the
 * Python interpreter they drive -- are exercised from more than one thread.
 * Prints a deterministic checksum the test runner compares against.
 *
 * Iteration count is deliberately small: this runs on every regression pass.
 */
#include <pthread.h>
#include <stdio.h>
#include <stdlib.h>

#ifndef NTHREADS
#define NTHREADS 4
#endif
#ifndef NITER
#define NITER 20000
#endif

static double acc[NTHREADS];

static void *work(void *arg)
{
   long id = (long)arg;
   double s = 0.0;
   for (long i = 1; i <= NITER; i++)
      s += 1.0 / (double)(i + id);
   acc[id] = s;
   return NULL;
}

int main(void)
{
   pthread_t t[NTHREADS];
   for (long i = 0; i < NTHREADS; i++)
      if (pthread_create(&t[i], NULL, work, (void *)i) != 0) {
         fprintf(stderr, "pthread_create failed\n");
         return 1;
      }
   for (int i = 0; i < NTHREADS; i++)
      pthread_join(t[i], NULL);

   double tot = 0;
   for (int i = 0; i < NTHREADS; i++)
      tot += acc[i];
   printf("threads=%d total=%.6f\n", NTHREADS, tot);
   return 0;
}
