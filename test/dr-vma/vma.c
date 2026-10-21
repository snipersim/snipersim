/* VMA logging (record-trace --vma) under the SDE and DynamoRIO front ends,
 * for testing/test_apps.py, dr-vma.
 *
 * The program works, maps 1 MiB at a fixed address, works with it, unmaps it
 * and works again; each stretch of work is far longer than the 10000
 * instructions between snapshots.  The log must show the region in the
 * snapshots taken while it was mapped, and in none before or after. */
#include <stdio.h>
#include <sys/mman.h>

#define REGION ((void *)0x200000000UL)
#define SIZE   (1UL << 20)

static unsigned buf[4096];

static unsigned long work(int n)
{
   unsigned long sum = 0;
   for (int r = 0; r < n; r++)
      for (int i = 0; i < 4096; i++)
      {
         buf[i] = buf[i] * 3 + (unsigned)i;
         sum += buf[i] >> 3;
      }
   return sum;
}

int main(void)
{
   unsigned long s = work(5);
   char *p = mmap(REGION, SIZE, PROT_READ | PROT_WRITE, MAP_PRIVATE | MAP_ANONYMOUS | MAP_FIXED_NOREPLACE, -1, 0);
   if (p != REGION)
   {
      printf("vma mmap failed\n");
      return 1;
   }
   for (unsigned long i = 0; i < SIZE; i += 4096)
      p[i] = (char)i;
   s += work(5);
   munmap(p, SIZE);
   s += work(5);
   printf("vma done sum=%lu\n", s);
   return 0;
}
