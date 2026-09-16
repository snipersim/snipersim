/* K1: exercise every magic command the frontend reserves.
 *
 * MagicServer's switch ends in a default: that asserts on an unrecognised
 * command, so before K1 a guest built against the new sim_api.h took the
 * simulator down rather than running.  This issues all eight of the newly
 * reserved commands and prints what each returned.
 *
 * The stubs return 0 for now; the handlers land in K2 (the ROI barrier),
 * K5 (scheduling) and K6 (fast fault).  The point of the check is that
 * issuing them is survivable and that they are routed rather than
 * swallowed -- so this also issues one already-handled command,
 * SIM_CMD_THREAD_ID, whose non-stub answer proves the magic path itself
 * is live in this run rather than silently inert.
 */
#include <stdio.h>
#include <stdint.h>
#include "sim_api.h"

int main(void)
{
   uint64_t tid = SimGetThreadId();

   /* K2 -- per-thread ROI barrier. */
   uint64_t expect   = SimRoiExpect(1);
   uint64_t tstart   = SimThreadRoiStart(1);
   uint64_t tend     = SimThreadRoiEnd(1);
   uint64_t waitroi  = SimWaitForRoi();

   /* K5 -- live-app scheduling. */
   uint64_t ctxto    = SimContextSwitchTo(0);
   uint64_t appid    = SimGetInjectedAppId();

   /* K6 -- fast-fault replay. */
   uint64_t argv[7]  = { 27, 0, 0, 0, 0, 0, 0 };
   uint64_t fault    = SimFastFault(7, (unsigned long)argv);
   uint64_t charge   = SimSetFastFaultCharge(0);

   printf("magic_k1 thread_id=%llu\n", (unsigned long long)tid);
   printf("magic_k1 roi_expect=%llu thread_roi_start=%llu "
          "thread_roi_end=%llu wait_for_roi=%llu\n",
          (unsigned long long)expect, (unsigned long long)tstart,
          (unsigned long long)tend, (unsigned long long)waitroi);
   printf("magic_k1 context_switch_to=%llu injected_app_id=%llu\n",
          (unsigned long long)ctxto, (unsigned long long)appid);
   printf("magic_k1 fast_fault=%llu set_fast_fault_charge=%llu\n",
          (unsigned long long)fault, (unsigned long long)charge);
   printf("magic_k1 survived all eight reserved commands\n");
   return 0;
}
