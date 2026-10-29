/* One use of time, chosen by argv[1], after some work: none, a poll with a
 * zero timeout (no wait), two rdtsc reads, the vDSO's clock_gettime, the
 * clock_gettime system call, a nanosleep, a poll with a timeout.
 * test/dr-refuse-time: under the DynamoRIO front end with -refuse_time, as
 * on an ISA where it does not keep the guest's time, the first three run
 * (the counter reads the instructions run) and the others end the run. */
#define _GNU_SOURCE
#include <poll.h>
#include <stdio.h>
#include <string.h>
#include <sys/syscall.h>
#include <time.h>
#include <unistd.h>
#include <x86intrin.h>

int main(int argc, char **argv)
{
   const char *m = argc > 1 ? argv[1] : "none";
   volatile unsigned long s = 0;
   for (unsigned long i = 0; i < 100000; i++)
      s += i;
   struct timespec ts = { 0, 0 };
   if (!strcmp(m, "vdso"))
      clock_gettime(CLOCK_MONOTONIC, &ts);
   else if (!strcmp(m, "raw"))
      syscall(SYS_clock_gettime, CLOCK_REALTIME, &ts);
   else if (!strcmp(m, "rdtsc"))
   {
      unsigned long long a = __rdtsc();
      for (int i = 0; i < 1000; i++)
         s += i;
      unsigned long long b = __rdtsc();
      printf("time_uses: tsc=%llu dtsc=%llu\n", a, b - a);
   }
   else if (!strcmp(m, "sleep"))
   {
      struct timespec d = { 0, 1000000 };
      nanosleep(&d, NULL);
   }
   else if (!strcmp(m, "poll0"))
      poll(NULL, 0, 0);
   else if (!strcmp(m, "poll"))
      poll(NULL, 0, 5);
   printf("time_uses: %s done\n", m);
   return 0;
}
