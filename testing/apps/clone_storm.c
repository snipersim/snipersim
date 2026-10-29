/* clone_storm: threads created at once by several threads, joined.
 *
 * Four threads each create four children at the same moment, then join
 * them and check what each wrote.  A front end that hands a new thread its
 * clear-tid word (the one pthread_join waits on) in the order clones were
 * made, rather than by the thread's own TLS, can give a child its
 * sibling's: at its exit it wakes the sibling's joiner, which reads a
 * result never written.  testing/test_qemu.py user-clone-storm runs it live.
 * Prints "clone_storm: ok <sum>" or "clone_storm: bad ...". */
#include <pthread.h>
#include <stdio.h>

#define PARENTS 4
#define CHILDREN 4

static pthread_barrier_t start;
static volatile long result[PARENTS][CHILDREN];

struct arg { int p, c; };

static void *child(void *a)
{
   const struct arg *x = a;
   volatile long s = 0;
   for (long i = 0; i < 20000 + 3000 * x->c; i++)
      s += i;
   result[x->p][x->c] = s + 1;
   return NULL;
}

static void *parent(void *a)
{
   const int p = (int)(long)a;
   pthread_t t[CHILDREN];
   struct arg args[CHILDREN];
   pthread_barrier_wait(&start);
   for (int c = 0; c < CHILDREN; c++)
   {
      args[c].p = p;
      args[c].c = c;
      pthread_create(&t[c], NULL, child, &args[c]);
   }
   long bad = 0;
   for (int c = 0; c < CHILDREN; c++)
   {
      pthread_join(t[c], NULL);
      if (!result[p][c])
         bad++;
   }
   return (void *)bad;
}

int main(void)
{
   pthread_t t[PARENTS];
   pthread_barrier_init(&start, NULL, PARENTS);
   for (long p = 0; p < PARENTS; p++)
      pthread_create(&t[p], NULL, parent, (void *)p);
   long bad = 0, sum = 0;
   for (int p = 0; p < PARENTS; p++)
   {
      void *r;
      pthread_join(t[p], &r);
      bad += (long)r;
   }
   for (int p = 0; p < PARENTS; p++)
      for (int c = 0; c < CHILDREN; c++)
         sum += result[p][c];
   if (bad)
      printf("clone_storm: bad %ld joins returned before their thread had written\n", bad);
   else
      printf("clone_storm: ok %ld\n", sum);
   return bad != 0;
}
