/* A work queue of irregular tasks: no barriers and no outer loop to cut on.
 *
 * The third shape in the set, after phased.c (barrier-delimited) and looped.c
 * (loop-delimited).  Task-parallel code removes both of the structures the
 * other two provide:
 *
 *   - there is no barrier inside the measured region, as in looped.c;
 *   - and there is no per-thread outer loop whose back edge means anything
 *     either, because a thread's next task has nothing to do with its last.
 *     The only boundary that carries information is where one task ends and
 *     the next begins.
 *
 * The tasks are deliberately irregular in both cost and kind: each is either
 * memory-bound or compute-bound, with a size drawn from a fixed seed, so the
 * work a thread happens to pick up is uneven and unpredictable.  A sampler
 * that assumes successive work resembles itself -- which is a reasonable
 * assumption in the other two workloads -- is wrong here.
 *
 * Determinism despite a work queue: which thread runs which task depends on
 * scheduling, so results are written to a per-task slot and summed in task
 * order at the end.  The checksum is then independent of interleaving, while
 * the execution genuinely is not.
 */
#include <pthread.h>
#include <stdint.h>
#include <stdio.h>
#include <stdlib.h>

#include "sim_api.h"

#ifndef NTHREADS
#define NTHREADS 4
#endif
#ifndef NTASKS
#define NTASKS 48
#endif
#ifndef CHASE_SLOTS
#define CHASE_SLOTS (1 << 21)   /* 16 MiB shared: past a typical LLC */
#endif
/* Scale knobs: a task's cost is a multiple of these, so the mix stays the same
 * shape as the workload is made bigger or smaller. */
#ifndef CHASE_UNIT
#define CHASE_UNIT 20000
#endif
#ifndef COMPUTE_UNIT
#define COMPUTE_UNIT 40000
#endif

enum { TASK_MEMORY, TASK_COMPUTE };

struct Task
{
   int      kind;
   long     units;      /* 1..4, so tasks differ in cost as well as in kind */
};

static struct Task tasks[NTASKS];
static double      task_result[NTASKS];
static uint64_t    task_visited[NTASKS];

static pthread_mutex_t queue_lock = PTHREAD_MUTEX_INITIALIZER;
static int next_task = 0;

static pthread_barrier_t gate;

/* One buffer, shared by every thread.
 *
 * Per-thread buffers made a memory task's result depend on which thread
 * happened to take it, so the reported total moved between runs while the work
 * did not.  Sharing is also the more honest shape for task-parallel code: the
 * tasks are operating on common data, and the cache pressure between them is
 * part of what is being modelled. */
static size_t *chase;

static uint32_t lcg(uint32_t *s)
{
   *s = (*s * 1664525u) + 1013904223u;
   return *s;
}

/* Build a single cycle visiting every slot exactly once.
 *
 * A full-period LCG over 0..n-1 is a permutation that is one n-cycle, so
 * a[x] = (A*x + C) mod n is the cycle, written in a single pass with no
 * scratch allocation.  Hull-Dobell gives full period for a power-of-two
 * modulus when C is odd and A = 1 (mod 4).  See the note in phased.c: the
 * Fisher-Yates this replaces ran before the region of interest and dominated
 * the fixed cost of a run.
 */
static void build_cycle(size_t *a, size_t n, uint32_t seed)
{
   const uint64_t A = 1664525u, C = 1013904223u;
   const size_t mask = n - 1;        /* n is a power of two */
   size_t x = seed & mask;

   for (size_t i = 0; i < n; i++)
   {
      size_t next = (size_t)((A * (uint64_t)x + C) & mask);
      a[x] = next;
      x = next;
   }
}

static int take_task(void)
{
   int t;
   pthread_mutex_lock(&queue_lock);
   t = (next_task < NTASKS) ? next_task++ : -1;
   pthread_mutex_unlock(&queue_lock);
   return t;
}

static void *work(void *arg)
{
   size_t *a = chase;
   size_t p;
   int t;

   (void)arg;

   pthread_barrier_wait(&gate);
   pthread_barrier_wait(&gate);

   while ((t = take_task()) >= 0)
   {
      const struct Task *task = &tasks[t];

      if (task->kind == TASK_MEMORY)
      {
         const long steps = task->units * CHASE_UNIT;
         /* Start each task at a fixed point in the cycle.  Carrying the
          * position across tasks made where a task ended depend on which
          * thread happened to run it, so the reported total moved from run to
          * run even though the work did not. */
         p = 0;
         for (long i = 0; i < steps; i++)
            p = a[p];
         task_visited[t] = p;
      }
      else
      {
         const long iters = task->units * COMPUTE_UNIT;
         double x = 1.0 + (double)t;
         for (long i = 0; i < iters; i++)
            x = x * 0.9999999 + 1e-7;
         task_result[t] = x;
      }
   }

   pthread_barrier_wait(&gate);   /* every task's result is stored */
   return NULL;
}

int main(void)
{
   pthread_t th[NTHREADS];
   uint32_t s = 99991u;
   double total = 0.0;
   uint64_t seen = 0;

   /* Fixed seed: the same mix of tasks every run, in the same order.
    *
    * Bit 16, not bit 0.  The low bit of an LCG modulo 2^32 has period two, and
    * because two values are drawn per task the kind would sample every other
    * output and come out identical for every task -- which it did: the first
    * version of this loop produced 48 compute tasks and no memory ones. */
   for (int i = 0; i < NTASKS; i++)
   {
      tasks[i].kind  = ((lcg(&s) >> 16) & 1) ? TASK_MEMORY : TASK_COMPUTE;
      tasks[i].units = 1 + (long)((lcg(&s) >> 16) % 4u);
   }

   pthread_barrier_init(&gate, NULL, NTHREADS + 1);
   chase = malloc(CHASE_SLOTS * sizeof(size_t));
   if (!chase)
   {
      fprintf(stderr, "tasked: out of memory\n");
      return 1;
   }
   build_cycle(chase, CHASE_SLOTS, 777u);

   for (long i = 0; i < NTHREADS; i++)
      pthread_create(&th[i], NULL, work, (void *)i);

   pthread_barrier_wait(&gate);
   SimRoiStart();
   pthread_barrier_wait(&gate);

   pthread_barrier_wait(&gate);

   /* Summed in task order, not completion order. */
   for (int i = 0; i < NTASKS; i++)
   {
      total += task_result[i];
      seen  += task_visited[i];
   }
   /* The results are stored before the last gate, and main sums and prints
    * them after it, still inside the ROI: whatever happens to the process
    * once the region is closed and the workers exit -- run-sniper used to
    * stop the program there, taken for a crash inside SDE, until it waited
    * for the front end -- the
    * checksum is already out, and the sampled run can be compared with the
    * full one. */
   printf("tasked: threads=%d tasks=%d checksum=%.6f visited=%llu\n",
          NTHREADS, NTASKS, total, (unsigned long long)seen);
   fflush(stdout);
   SimRoiEnd();

   for (int i = 0; i < NTHREADS; i++)
      pthread_join(th[i], NULL);

   pthread_barrier_destroy(&gate);
   free(chase);
   return 0;
}
