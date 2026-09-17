/* More software threads than cores, with a barrier between rounds -- the
 * shape of fft -p 8 on -n 4, which used to deadlock at the fifth thread.
 *
 * Eight threads each do a little work, then meet at a barrier, several
 * times.  With four cores the scheduler has to time-slice: threads park at
 * the barrier (futex) and others take their cores; a new thread's recorder
 * must be able to start while its trace thread has no core yet.  The
 * check requires the run to finish and every thread to have run. */
#include <stdio.h>
#include <stdlib.h>
#include <pthread.h>

#define NTHREADS 8
#define ROUNDS 4

static pthread_barrier_t barrier;
static volatile unsigned long sums[NTHREADS];

static void *worker(void *arg)
{
   long id = (long)arg;
   unsigned long s = 0;
   for (int r = 0; r < ROUNDS; r++)
   {
      for (unsigned long i = 0; i < 20000; i++)
         s += (i * (id + 1)) ^ r;
      pthread_barrier_wait(&barrier);
   }
   sums[id] = s;
   return NULL;
}

int main(void)
{
   pthread_t th[NTHREADS];
   pthread_barrier_init(&barrier, NULL, NTHREADS);
   for (long i = 1; i < NTHREADS; i++)
      pthread_create(&th[i], NULL, worker, (void *)i);
   worker((void *)0L);
   for (long i = 1; i < NTHREADS; i++)
      pthread_join(th[i], NULL);
   unsigned long total = 0;
   for (int i = 0; i < NTHREADS; i++)
      total += sums[i];
   printf("oversub done threads=%d total=%lu\n", NTHREADS, total);
   return 0;
}
