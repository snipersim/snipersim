/* roi_time: the guest's clock after its region of interest.
 *
 * After SimRoiEnd() a live run goes on counting the guest's instructions to
 * the simulator, so the simulated clock keeps moving: this guest spins on
 * clock_gettime(CLOCK_MONOTONIC) until 1 ms has passed, and checks that the
 * TSC moved too.  With the clock frozen (as it was) the spin would never end,
 * so it gives up after 50 million reads and exits 1.  testing/test_qemu.py
 * user-roi-time runs it live.  Prints
 *
 *   roi_time: elapsed_ns=N reads=R tsc_moved=0|1
 */
#include <stdio.h>
#include <stdint.h>
#include <time.h>
#include "sim_api.h"
#if defined(__x86_64__)
#include <x86intrin.h>
static uint64_t tsc(void) { return __rdtsc(); }
#else
static uint64_t tsc(void) { return 0; }
#endif

static uint64_t now_ns(void)
{
   struct timespec ts;
   clock_gettime(CLOCK_MONOTONIC, &ts);
   return (uint64_t)ts.tv_sec * 1000000000u + (uint64_t)ts.tv_nsec;
}

int main(void)
{
   volatile uint64_t x = 0;
   SimRoiStart();
   for (int i = 0; i < 100000; i++)
      x += i;
   SimRoiEnd();
   const uint64_t t0 = now_ns(), c0 = tsc();
   uint64_t reads = 0, t = t0;
   while (t - t0 < 1000000 && reads < 50000000)
   {
      t = now_ns();
      reads++;
   }
   const int tsc_moved = tsc() > c0;
   printf("roi_time: elapsed_ns=%llu reads=%llu tsc_moved=%d\n", (unsigned long long)(t - t0), (unsigned long long)reads, tsc_moved);
   return (t - t0 >= 1000000 && x) ? 0 : 1;
}
