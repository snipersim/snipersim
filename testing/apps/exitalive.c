/* The main thread returns -- exit_group -- while its other threads are still
 * running: four workers compute 3M iterations each and are never joined.
 *
 * For the simulator the exit_group ends the application: the workers' trace
 * threads are stopped and close their channels while the process, and the
 * workers in it, run on until the exit completes.  Live under the SDE front
 * end this hung every time: a worker parked in the simulator's barrier was
 * never let out (at -n 5), and a worker that never had a core left its
 * recorder thread waiting for an answer for ever (at -n 2).  Under QEMU the
 * workers' writes into their closed channels killed QEMU with SIGPIPE, and
 * its exit freed a working thread's streams under it.
 *
 * Used by test/exitalive (SDE) and testing/test_qemu.py's user-exit-alive.
 */
#include <pthread.h>
#include <stdio.h>

#define NTHREADS 4

static volatile double sink;

static void *work(void *arg)
{
   double x = 1;
   for (long i = 0; i < 3000000; i++)
      x = x * 0.9999999 + 1e-7;
   sink = x;
   return arg;
}

int main(void)
{
   pthread_t th[NTHREADS];
   for (long i = 0; i < NTHREADS; i++)
      pthread_create(&th[i], NULL, work, NULL);

   volatile double y = 0;
   for (long i = 0; i < 100000; i++)
      y += i;
   printf("exitalive: main returns\n");
   fflush(stdout);
   return 0;
}
