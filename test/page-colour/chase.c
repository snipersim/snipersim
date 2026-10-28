/* A pointer chase over 192 KiB, three quarters of gainestown's private
   256 KiB L2, repeated: alone it fits the L2.  test_apps runs it alone and as
   two applications on two cores, which must each miss no more than alone --
   each application's pages have to cover the L2's page colours evenly
   whatever order the two fault in (the physical allocator's per-core runs). */
#include <stdio.h>
#include <stdlib.h>
#include <stdint.h>
#include "sim_api.h"
#define SLOTS (192 * 1024 / 64)        /* one slot per 64-byte line */
#define LAPS  40
struct node { uint32_t next; uint32_t pad[15]; };
int main(void)
{
   struct node *a = aligned_alloc(4096, SLOTS * sizeof(struct node));
   uint32_t *perm = malloc(SLOTS * sizeof(uint32_t));
   uint64_t s = 12345;
   for (uint32_t i = 0; i < SLOTS; i++) perm[i] = i;
   for (uint32_t i = SLOTS - 1; i > 0; i--) { s = s * 6364136223846793005ull + 1442695040888963407ull; uint32_t j = (uint32_t)((s >> 33) % (i + 1)); uint32_t t = perm[i]; perm[i] = perm[j]; perm[j] = t; }
   for (uint32_t i = 0; i < SLOTS; i++) a[perm[i]].next = perm[(i + 1) % SLOTS];
   uint32_t p = perm[0];
   SimRoiStart();
   for (int lap = 0; lap < LAPS; lap++)
      for (uint32_t i = 0; i < SLOTS; i++) p = a[p].next;
   SimRoiEnd();
   printf("chase %u\n", p);
   return 0;
}
