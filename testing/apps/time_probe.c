/* time_probe: the guest's time through libc, under every front end
 * (testing/test_qemu.py, time-start-default).
 *
 * clock_gettime, gettimeofday and the TSC, read twice around a loop of
 * 100,000 iterations.  Recorded to a file -- no simulator to answer -- every
 * front end gives the same model: one instruction per cycle at an assumed
 * 1 GHz, counted from the thread's start, from Sniper's time zero
 * (osemu/time_start, 1337000000 seconds by default; Sift::TIME_START_DEFAULT
 * in sift/sift_format.h).  Never the host's clock.  It prints
 *
 *   time_probe: realtime=S.N mono=S.N tod=S.U dreal=D dtsc=T
 *
 * the first readings, the CLOCK_REALTIME difference over the loop in
 * nanoseconds and the TSC's in cycles (x86-64; 0 elsewhere).
 */
#include <stdio.h>
#include <stdint.h>
#include <time.h>
#include <sys/time.h>
#if defined(__x86_64__)
#include <x86intrin.h>
static uint64_t tsc(void) { return __rdtsc(); }
#else
static uint64_t tsc(void) { return 0; }
#endif

int main(void)
{
   struct timespec r0, m0, r1;
   struct timeval tv;
   clock_gettime(CLOCK_REALTIME, &r0);
   clock_gettime(CLOCK_MONOTONIC, &m0);
   gettimeofday(&tv, NULL);
   const uint64_t c0 = tsc();
   volatile uint64_t x = 0;
   for (int i = 0; i < 100000; i++)
      x += i;
   const uint64_t c1 = tsc();
   clock_gettime(CLOCK_REALTIME, &r1);
   const int64_t dreal = (int64_t)(r1.tv_sec - r0.tv_sec) * 1000000000 + (r1.tv_nsec - r0.tv_nsec);
   printf("time_probe: realtime=%ld.%09ld mono=%ld.%09ld tod=%ld.%06ld dreal=%lld dtsc=%llu\n",
          (long)r0.tv_sec, (long)r0.tv_nsec, (long)m0.tv_sec, (long)m0.tv_nsec,
          (long)tv.tv_sec, (long)tv.tv_usec, (long long)dreal, (unsigned long long)(c1 - c0));
   return x ? 0 : 1;
}
