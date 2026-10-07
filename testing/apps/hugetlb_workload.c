/* The hugetlbfs feature check's guest: one large anonymous mapping, walked
 * a few times.
 *
 * A hugetlbfs user asks for huge pages with mmap(MAP_HUGETLB) or a file on
 * a hugetlbfs mount, neither of which a SIFT trace records -- the recorder's
 * VMA log is the address ranges of /proc/self/maps and nothing else.  So the
 * simulator decides which VMAs the pool backs by a configured criterion
 * (perf_model/hugetlbfs/min_vma_mb), and this guest exists to give it a
 * VMA that qualifies: an 8 MB anonymous region, which the shared workload
 * (two buffers of 16 KB and 32 KB) never produces.  Every 4 KB page is
 * touched so the region is faulted in page by page, and the region is then
 * read back twice more at cache-line stride so the 2 MB translations are
 * hit in the TLB rather than only installed.
 *
 * mmap rather than malloc, so the region is its own VMA whatever glibc's
 * mmap threshold does; MAP_POPULATE deliberately not, since the simulated
 * page faults are the point.
 */
#include <stdio.h>
#include <stdint.h>
#include <sys/mman.h>

#define REGION_BYTES (8u << 20)
#define LINE 64
#define PASSES 3

int main(void)
{
   unsigned char *p = mmap(NULL, REGION_BYTES, PROT_READ | PROT_WRITE,
                           MAP_PRIVATE | MAP_ANONYMOUS, -1, 0);
   if (p == MAP_FAILED) {
      perror("mmap");
      return 1;
   }
   /* First pass writes: one fault per 4 KB page, in order. */
   for (uint32_t i = 0; i < REGION_BYTES; i += LINE)
      p[i] = (unsigned char)(i >> 6);
   uint64_t sum = 0;
   for (int pass = 0; pass < PASSES; pass++)
      for (uint32_t i = 0; i < REGION_BYTES; i += LINE)
         sum += p[i] + pass;
   printf("hugetlb_workload sum=%llu\n", (unsigned long long)sum);
   return 0;
}
