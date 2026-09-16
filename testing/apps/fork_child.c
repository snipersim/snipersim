/* K3: a guest that forks without execing.
 *
 * The recorder opens the child's SIFT FIFO from forkAfterInChild.  Virtuoso
 * drops that call, because a child that goes on to execve would otherwise
 * leave a stranded header in the FIFO and the post-execve recorder would
 * write a second one.  Here the call is kept, because nothing would open the
 * FIFO for a child that does not exec -- threads.cc opens only for
 * threadid > 0 -- and this is the shape that works today.
 *
 * Both processes have to produce output and both traces have to finish; a
 * child recording into nothing shows up as a missing line and a missing
 * trace, not as an error.
 */
#include <stdio.h>
#include <unistd.h>
#include <sys/wait.h>

#define PARENT_ITERS 200000L
#define CHILD_ITERS  100000L

static double spin(long iters, long seed)
{
   double s = 0.0;
   for (long i = 1; i <= iters; i++)
      s += 1.0 / (double)(i + seed);
   return s;
}

int main(void)
{
   double parent_sum = spin(PARENT_ITERS, 0);

   pid_t p = fork();
   if (p == 0)
   {
      printf("fork_child child sum=%.6f\n", spin(CHILD_ITERS, 1));
      return 0;
   }

   int status;
   waitpid(p, &status, 0);
   printf("fork_child parent sum=%.6f\n", parent_sum);
   return 0;
}
