/* touch_pages: a guest for the address-translation checks (testing/test_qemu.py,
 * riscv-user-translation and aarch64-user-translation).  It writes one byte to
 * every PAGES_PER_REGION-th 4 KiB page of REGIONS regions of 2 MiB each in a
 * static array, so the simulator sees a known number of first-touch faults in
 * a known number of leaf page tables, touches four pages of its stack (at the
 * top of the guest's address space), then exits with 100.  Freestanding:
 *
 *   clang --target=riscv64-linux-gnu  -march=rv64gc -mabi=lp64d -O1 \
 *   clang --target=aarch64-linux-gnu                               -O1 \
 *       -ffreestanding -nostdlib -static -fuse-ld=lld -Wl,-e,_start -o touch_pages touch_pages.c
 */
#include <stdint.h>

#define REGIONS          8                 /* 2 MiB regions of the array touched */
#define PAGES_PER_REGION 64                /* 4 KiB pages touched in each */
#define REGION_BYTES     (2u << 20)

static volatile unsigned char arena[REGIONS * REGION_BYTES] __attribute__((aligned(2u << 20)));

static long sys3(long n, long a, long b, long c)
{
#if defined(__aarch64__)
   register long a0 __asm__("x0") = a; register long a1 __asm__("x1") = b; register long a2 __asm__("x2") = c;
   register long a7 __asm__("x8") = n;
   __asm__ volatile("svc #0" : "+r"(a0) : "r"(a1), "r"(a2), "r"(a7) : "memory");
#else
   register long a0 __asm__("a0") = a; register long a1 __asm__("a1") = b; register long a2 __asm__("a2") = c;
   register long a7 __asm__("a7") = n;
   __asm__ volatile("ecall" : "+r"(a0) : "r"(a1), "r"(a2), "r"(a7) : "memory");
#endif
   return a0;
}

void _start(void)
{
   /* The stack too: QEMU places it at the top of the guest's address space, so
      this is the access that tells a 48-bit layout from a 39-bit one. */
   volatile unsigned char stack_pages[4 * 4096];
   for (unsigned i = 0; i < 4; i++)
      stack_pages[i * 4096 + 17] = (unsigned char)i;
   unsigned sum = stack_pages[17] + stack_pages[4096 + 17];
   for (unsigned r = 0; r < REGIONS; r++)
      for (unsigned p = 0; p < PAGES_PER_REGION; p++)
      {
         volatile unsigned char *b = &arena[r * REGION_BYTES + p * 4096u + (r ^ p) % 4096u];
         *b = (unsigned char)(r + p);
         sum += *b;
      }
   static const char msg[] = "touch_pages: 512 pages in 8 regions, 4 stack pages\n";
   sys3(64 /* write */, 1, (long)msg, sizeof(msg) - 1);
   sys3(94 /* exit_group */, sum ? 100 : 101, 0, 0);
   for (;;) {}
}
