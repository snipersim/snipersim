/* A program with work left after its region of interest, the shape of
 * testing/apps/phased.c: the workers leave at the end of the ROI while the
 * main thread goes on.
 *
 * At the end of the ROI the recorder ends the main thread's stream and the
 * simulation ends; the program runs on natively.  Two things went wrong
 * there.  run-sniper killed the front end the moment the simulator exited,
 * so whatever the program did after its ROI raced the simulator's wind-down,
 * and its output after the ROI came out or not by timing.  And the
 * recorder's thread-exit helpers closed a thread's writer while the Fini
 * callback of the thread that ended the process closed it too.
 *
 *   roi_exit join   main joins the workers, works on natively, then prints
 *   roi_exit exit   main prints and returns while the workers are leaving
 *
 * Either way the last line is the program's; it is printed after the ROI,
 * so it reaches the log only if the program ran to its end.
 */
#include <pthread.h>
#include <stdio.h>
#include <string.h>

#include "sim_api.h"

#define NTHREADS 4

static pthread_barrier_t gate;
static volatile double result[NTHREADS];

static void *work(void *arg)
{
   long id = (long)arg;
   double x = 1.0 + id;

   pthread_barrier_wait(&gate);        /* every thread is up */
   pthread_barrier_wait(&gate);        /* main has opened the region */
   for (long i = 0; i < 200000; i++)
      x = x * 0.9999999 + 1e-7;
   result[id] = x;
   pthread_barrier_wait(&gate);        /* main closes the region after this */
   return NULL;
}

int main(int argc, char **argv)
{
   int join = !(argc > 1 && strcmp(argv[1], "exit") == 0);
   pthread_t th[NTHREADS];
   double total = 0;

   pthread_barrier_init(&gate, NULL, NTHREADS + 1);
   for (long i = 0; i < NTHREADS; i++)
      pthread_create(&th[i], NULL, work, (void *)i);

   pthread_barrier_wait(&gate);
   SimRoiStart();
   pthread_barrier_wait(&gate);
   pthread_barrier_wait(&gate);
   for (int i = 0; i < NTHREADS; i++)
      total += result[i];
   SimRoiEnd();

   if (join)
   {
      for (int i = 0; i < NTHREADS; i++)
         pthread_join(th[i], NULL);
      /* native work after the region: a second or so under the recorder */
      volatile double y = 0;
      for (long i = 0; i < 20000000; i++)
         y += i;
   }

   printf("roi-exit: %s total=%.6f, ran to its end after the ROI\n", join ? "join" : "exit", total);
   fflush(stdout);
   return 0;
}
