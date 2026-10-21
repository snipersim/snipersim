/* Physical addresses in the trace (record-trace --pa, run-sniper --pa), for
 * testing/test_apps.py, pa.  Touches 64 pages of a buffer, so the trace has
 * 64 virtual pages whose physical frames it must carry.  Needs
 * CAP_SYS_ADMIN, without which Linux hides frame numbers in
 * /proc/self/pagemap; the case is skipped on such a host (and pa-refused
 * checks that run-sniper says so instead of quietly going on without). */
#include <stdio.h>

#define PAGES 64
static char buf[PAGES * 4096] __attribute__((aligned(4096)));

int main(void)
{
   unsigned long sum = 0;
   for (int r = 0; r < 4; r++)
      for (int p = 0; p < PAGES; p++)
      {
         buf[p * 4096 + r] = (char)(p + r);
         sum += buf[p * 4096];
      }
   printf("pa done sum=%lu buf=%p\n", sum, (void *)buf);
   return 0;
}
