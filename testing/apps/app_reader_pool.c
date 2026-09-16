/* K5a: the AppReaderPool and the magic commands that read it.
 *
 * SimGetInjectedAppId reports the application injected into the calling
 * TraceThread: -1 before anything is injected, and its app id afterwards.  A
 * kernel reads that and derives the thread id as app_id << 16, which is how
 * the reader pool is keyed, so the first injected application reports 1 and
 * is registered at 65536.
 *
 * SimContextSwitchTo is only exercised here through its miss path.  On a hit
 * it returns 42, which tells the SIFT reader to send no response record --
 * correct, because the caller is not meant to resume and the newly installed
 * stream runs next, but it means a guest switching on its own behalf is never
 * answered and blocks forever.  It is a kernel primitive.  The miss path must
 * answer normally, and that is what is checked: asking for a thread that does
 * not exist has to leave the caller running.
 */
#include <stdio.h>
#include <string.h>
#include "sim_api.h"

int main(void)
{
   char path[256];
   memset(path, 0, sizeof(path));
   strncpy(path, "test/sniper-in-sniper/fft.sift", sizeof(path) - 1);

   long before = (long)SimGetInjectedAppId();
   printf("pool before_inject atid=%ld\n", before);
   fflush(stdout);

   SimStartProcess((unsigned long)path);

   unsigned long after = SimGetInjectedAppId();
   printf("pool after_inject atid=%lu\n", after);
   fflush(stdout);

   /* A thread id nothing was ever registered under. */
   SimContextSwitchTo(0xdead);
   printf("pool survived_miss\n");
   fflush(stdout);

   double s = 0.0;
   for (long i = 1; i <= 50000; i++)
      s += 1.0 / (double)i;
   printf("pool done s=%.6f\n", s);
   return 0;
}
