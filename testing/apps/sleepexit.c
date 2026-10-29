/* sleepexit: one thread sleeps in a loop while another works, finishes, and
 * exits.
 *
 * Sniper emulates nanosleep: the sleeper is parked until simulated time
 * reaches its wake-up, and only the barrier moves simulated time, as the
 * running cores check in.  When the worker exited during one of the main
 * thread's sleeps, no core was left running, nothing advanced time to the
 * wake-up, and the run hung with every simulator thread idle -- under the SDE
 * and the QEMU front ends alike (ThreadManager::onThreadExit).
 *
 * Usage: sleepexit <worker iterations> [<worker sleeps every N iterations>]
 * The main thread sleeps 100 us at a time until the worker is done, then
 * joins it.  test/sleepexit runs it under the SDE front end (test_apps.py),
 * test_qemu.py user-sleep-exit under QEMU. */
#include <pthread.h>
#include <stdio.h>
#include <stdlib.h>
#include <time.h>

static volatile unsigned long sink;
static volatile int done;
static unsigned long iterations, sleep_every;

static void *worker(void *arg)
{
   struct timespec ts = { 0, 50000 };
   for (unsigned long i = 0; i < iterations; i++)
   {
      sink += i;
      if (sleep_every && i % sleep_every == sleep_every - 1)
         nanosleep(&ts, NULL);
   }
   done = 1;
   return arg;
}

int main(int argc, char **argv)
{
   setvbuf(stdout, NULL, _IOLBF, 0);
   iterations = argc > 1 ? strtoul(argv[1], NULL, 0) : 2000;
   sleep_every = argc > 2 ? strtoul(argv[2], NULL, 0) : 0;

   pthread_t t;
   pthread_create(&t, NULL, worker, NULL);

   struct timespec ts = { 0, 100000 };
   int n = 0;
   while (!done && n < 100000)
   {
      nanosleep(&ts, NULL);
      n++;
   }
   printf("sleepexit: %d sleeps, done=%d\n", n, done);
   pthread_join(t, NULL);
   puts("sleepexit: joined");
   return 0;
}
