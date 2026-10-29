/* rep movsb with a count of zero, a third of the time: QEMU runs such a
 * pass with no memory access, and the plugin recorded it with none of the
 * two addresses Sniper's decoder expects of movsb, which asserted
 * (test_qemu user-rep-zero).  Prints the sum of a[0] after each copy:
 * 'h' (104) after every one but the first, which copies nothing: 999 x 104
 * = 103896. */
#include <stdio.h>

int main(void)
{
   static char a[64], b[64] = "hello";
   unsigned long s = 0;
   for (int i = 0; i < 1000; i++)
   {
      void *d = a, *src = b;
      unsigned long n = (i % 3 == 0) ? 0 : 5;
      __asm__ volatile("rep movsb" : "+D"(d), "+S"(src), "+c"(n) : : "memory");
      s += a[0];
   }
   printf("rep_zero: %lu %s\n", s, a);
   return 0;
}
