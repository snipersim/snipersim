/* roi_wait: a worker blocked on a condition variable when the region of
 * interest ends, woken by the main thread only after it.
 *
 * Live under the QEMU front end the worker's futex wait is held in the
 * simulator's model of the futex, so its wake has to go there too, after the
 * ROI as before it: run natively, it never reached the waiter, and the run
 * hung every time.  So must the exiting worker's wake of its joiner (its tid
 * word).  testing/test_qemu.py user-roi-wait runs it; it prints its native
 * result. */
#include <pthread.h>
#include <stdio.h>
#include <unistd.h>
#include "sim_api.h"

static pthread_mutex_t m = PTHREAD_MUTEX_INITIALIZER;
static pthread_cond_t c = PTHREAD_COND_INITIALIZER;
static int go = 0, waiting = 0;

static void *worker(void *a)
{
   pthread_mutex_lock(&m);
   waiting = 1;
   while (!go)
      pthread_cond_wait(&c, &m);
   pthread_mutex_unlock(&m);
   return a;
}

int main(void)
{
   pthread_t t;
   volatile unsigned long x = 0;
   SimRoiStart();
   pthread_create(&t, NULL, worker, NULL);
   for (int done = 0; !done; )
   {
      pthread_mutex_lock(&m); done = waiting; pthread_mutex_unlock(&m);
      for (int i = 0; i < 1000; i++) x += i;
   }
   for (unsigned long i = 0; i < 200000; i++) x += i;   /* the worker is in its futex wait by now */
   SimRoiEnd();
   usleep(100000);
   pthread_mutex_lock(&m); go = 1; pthread_cond_signal(&c); pthread_mutex_unlock(&m);
   pthread_join(t, NULL);
   printf("roi_wait: done%s\n", x ? "" : " (no work)");   /* x depends on the timing: not printed */
   return 0;
}
