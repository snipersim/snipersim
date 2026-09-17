/* The per-core event queue, driven from a guest with no kernel.
 *
 * SimReceiveMessage is a wait, not a probe: a core whose queue is empty is
 * parked through the ThreadManager like an idle CPU halted until an
 * interrupt, and whoever posts to that queue wakes it.  The one producer a
 * guest can drive itself is injection: SimStartProcess posts new_thread to
 * the injecting core's queue.  So the guest injects an application and
 * receives, and the check pins what arrives -- argc, the protocol code and
 * the app thread id -- and that the handler overwrote the sentinel.
 *
 * The guest stops there.  Running the injected application means handling
 * its page faults, which is the kernel's job; the kernel-driven two-core
 * check (mimicos_two_app_smp) covers the rest of the protocol: parking on
 * an empty queue, the wake on shutdown, and the exit reports.
 *
 * The K4 version of this test probed an empty queue and expected argc=0
 * back at once.  That was the stopgap before anything posted events; a
 * guest that asks an empty queue with no producer now parks, and Sniper
 * reports the idle deadlock rather than hanging. */
#include <stdio.h>
#include <stdint.h>
#include "sim_api.h"

int main(void)
{
   char path[] = "test/sniper-in-sniper/fft.sift";   /* array: no link-time constant in the magic */
   SimStartProcess((unsigned long)path);
   unsigned long app_id = SimGetInjectedAppId();
   printf("mimicos_event injected app_id=%lu\n", app_id);

   int argc_slot = 0x5A5A5A5A;                  /* sentinel: the handler must write */
   uint64_t argv_slot[8] = { 0 };
   SimReceiveMessage((unsigned long)&argc_slot, (unsigned long)argv_slot);
   printf("mimicos_event new_thread argc=%d code=%lu arg=%lu\n", argc_slot,
          (unsigned long)argv_slot[0], (unsigned long)argv_slot[1]);

   printf("mimicos_event done\n");
   return 0;
}
