/* The application the tool runs: its first thread calls block_here(), where
 * the tool blocks in fopen() on a FIFO and never returns; the main thread
 * waits for the tool's marker, then calls probe(), where the tool tries a
 * second fopen().  Neither function does anything itself. */
#include <pthread.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <unistd.h>

void __attribute__((noinline)) block_here(void) { __asm__ volatile("" ::: "memory"); }
void __attribute__((noinline)) probe(void) { __asm__ volatile("" ::: "memory"); }

static void raw(const char *m) { ssize_t r = write(2, m, strlen(m)); (void)r; }
static void *blocked_thread(void *a) { raw("app: thread calling block_here()\n"); block_here(); raw("app: block_here() returned?!\n"); return a; }

int main(void)
{
   const char *d = getenv("FIFO_DIR");
   char marker[600];
   snprintf(marker, sizeof(marker), "%s/blocker-started", d ? d : ".");
   pthread_t t;
   pthread_create(&t, 0, blocked_thread, 0);
   raw("app: pthread_create returned; polling for the marker\n");
   int polls = 0;
   while (access(marker, F_OK) != 0)
   {
      if (++polls % 50 == 1)
         raw("app: still polling for the marker (heartbeat)\n");
      usleep(10 * 1000);
   }
   usleep(200 * 1000);
   raw("app: marker seen, calling probe()\n");
   probe();
   raw("app: probe() returned -- the tool did not stop the process\n");
   if (getenv("APP_LINGER"))       /* stay alive for tools whose checks run on their own threads */
      sleep(10);
   return 0;
}
