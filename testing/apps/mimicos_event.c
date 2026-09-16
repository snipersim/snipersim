/* K4: the SimReceiveMessage consumer reads a per-core event queue.
 *
 * The queue replaced a single shared MimicOSMessage slot.  With one slot a
 * core asking for an event got the last one posted on any core, and whatever
 * happened to be left there when nothing had been posted at all.
 *
 * Nothing posts events yet -- every producer (new_thread, thread_exit,
 * quantum_expired, and the page-fault path) sits behind the userspace kernel
 * that arrives later -- so what is checked here is the consumer: that it runs
 * against the queue and reports "no event" as a definite answer rather than
 * leaving the guest's buffer alone.
 *
 * The sentinel is what makes that observable.  The guest writes 0x5A5A5A5A
 * into argc and asks for an event; the handler must overwrite it with 0.  A
 * handler that never ran, or that returned without writing, leaves the
 * sentinel in place and the check fails.
 */
#include <stdio.h>
#include <stdint.h>
#include "sim_api.h"

#define SENTINEL 0x5A5A5A5A

int main(void)
{
   volatile int argc_slot = SENTINEL;
   uint64_t argv_slot[8] = { 0 };

   SimReceiveMessage((unsigned long)&argc_slot, (unsigned long)argv_slot);
   printf("mimicos_event first argc=%d\n", argc_slot);

   /* Ask again: an empty queue must keep answering the same way rather than
      draining into something stale. */
   argc_slot = SENTINEL;
   SimReceiveMessage((unsigned long)&argc_slot, (unsigned long)argv_slot);
   printf("mimicos_event second argc=%d\n", argc_slot);

   printf("mimicos_event done\n");
   return 0;
}
