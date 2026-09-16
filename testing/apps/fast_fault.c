/* K6: fast-fault replay, and the magic ABI it depends on.
 *
 * The guest fills a seven-word payload and passes its address; the simulator
 * must read back exactly those words and charge the profile's mean cost.
 *
 * The values are distinctive on purpose.  This is the check that would have
 * caught the two defects in the magic macros:
 *
 *   - the asm wrote MAGIC_REG_A from its first input before reading the
 *     later ones, with the result register not marked earlyclobber, so the
 *     compiler could place the payload pointer in that same register and the
 *     first mov destroyed it.  The simulator then read from the command
 *     number as if it were an address, and every word came back zero.
 *   - the asm declared no "memory" clobber, so stores into a local payload
 *     were dead as far as the compiler could see, and free to drop.
 *
 * Both are invisible unless something checks the values that arrive, which
 * is why the payload is asserted word for word rather than just the call
 * surviving.
 */
#include <stdio.h>
#include <stdint.h>
#include "sim_api.h"

#define CHARGE_FAULTS 10
#define CHARGE_CYCLES 1000000

int main(void)
{
   uint64_t payload[7] = {
      27,          /* protocol marker, redundant with the command */
      42,          /* fp_id      */
      0xCAFE000,   /* frame_pa   */
      5000,        /* mean_cycles */
      4,           /* trace_len  */
      0,           /* trace_ptr  */
      7,           /* cursor     */
   };

   SimFastFault(7, payload);
   printf("fast_fault probe sent\n");
   fflush(stdout);

   /* Charge a known amount of time, so the check can assert the cost lands
      and not merely that the payload arrived.  Ten faults of a million
      cycles is ten million, against a baseline run of about two hundred
      thousand -- far too large to be anything else.
      
      The first version of this handler advanced ShmemPerfModel's elapsed
      time, which moved nothing at all: that clock belongs to the memory
      subsystem rather than to the core's instruction timeline.  A check
      that only looked at the payload could not tell. */
   uint64_t charged[7] = { 27, 43, 0, CHARGE_CYCLES, 0, 0, 0 };
   for (int i = 0; i < CHARGE_FAULTS; i++)
      SimFastFault(7, charged);
   printf("fast_fault charged %d x %d cycles\n", CHARGE_FAULTS, CHARGE_CYCLES);
   fflush(stdout);

   /* The charge the MMU should queue per fault once a profile is trained. */
   SimSetFastFaultCharge(1234);
   printf("fast_fault charge set\n");
   fflush(stdout);

   double s = 0.0;
   for (long i = 1; i <= 50000; i++)
      s += 1.0 / (double)i;
   printf("fast_fault done s=%.6f\n", s);
   return 0;
}
