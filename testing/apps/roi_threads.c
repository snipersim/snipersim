/* roi_threads: threads, joins and a timed wait after the region of interest.
 *
 * A live run stays live after SimRoiEnd(): threads started after it are the
 * simulator's too, their joins wait in its futex model, and a timed wait is
 * timed in simulated time -- nothing is handed back to the host's kernel
 * because the ROI ended.  After the ROI this guest starts a thread that
 * starts another, joins both, then waits on a condition variable nobody
 * signals until an absolute CLOCK_REALTIME deadline 10 ms ahead, and checks
 * that the wait timed out at the deadline.  testing/test_qemu.py
 * user-roi-threads runs it live.  Prints
 *
 *   roi_threads: joined=2 timedwait=ETIMEDOUT waited_ns=N
 */
#include <errno.h>
#include <pthread.h>
#include <stdio.h>
#include <stdint.h>
#include <time.h>
#include "sim_api.h"

static volatile int ran = 0;

static void *grandchild(void *a) { __sync_fetch_and_add(&ran, 1); return a; }

static void *child(void *a)
{
   pthread_t g;
   pthread_create(&g, NULL, grandchild, NULL);
   pthread_join(g, NULL);
   __sync_fetch_and_add(&ran, 1);
   return a;
}

int main(void)
{
   volatile uint64_t x = 0;
   SimRoiStart();
   for (int i = 0; i < 100000; i++)
      x += i;
   SimRoiEnd();

   pthread_t c;
   pthread_create(&c, NULL, child, NULL);
   pthread_join(c, NULL);

   pthread_mutex_t m = PTHREAD_MUTEX_INITIALIZER;
   pthread_cond_t cv = PTHREAD_COND_INITIALIZER;
   struct timespec start, deadline, end;
   clock_gettime(CLOCK_REALTIME, &start);
   deadline = start;
   deadline.tv_nsec += 10000000;
   if (deadline.tv_nsec >= 1000000000) { deadline.tv_sec++; deadline.tv_nsec -= 1000000000; }
   pthread_mutex_lock(&m);
   int rc = pthread_cond_timedwait(&cv, &m, &deadline);
   pthread_mutex_unlock(&m);
   clock_gettime(CLOCK_REALTIME, &end);
   const long long waited = (long long)(end.tv_sec - start.tv_sec) * 1000000000 + (end.tv_nsec - start.tv_nsec);
   printf("roi_threads: joined=%d timedwait=%s waited_ns=%lld\n", ran, rc == ETIMEDOUT ? "ETIMEDOUT" : "other", waited);
   return (ran == 2 && rc == ETIMEDOUT && x) ? 0 : 1;
}
