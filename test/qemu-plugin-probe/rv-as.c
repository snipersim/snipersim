/* Two address spaces with different code and data at one virtual address,
 * for the RISC-V full-system trace (the x86 version is bootas.S).
 *
 * Machine mode sets up PMP (S-mode may use all memory) and Sv39 tables, then
 * drops to supervisor mode.  The tables identity-map the first GiB (devices)
 * and the GiB from 0x80000000 (RAM), and map VA 0x40000000 (a 2 MiB page) to
 * PA 0x80200000 in A and 0x80400000 in B.  Each page gets different code --
 * B's first instruction is compressed -- and each stores its own value at VA
 * 0x400007f0.  Supervisor mode switches satp between A and B and calls VA
 * 0x40000000, NROUNDS times each; the test finisher reports 0x5555 when the
 * calls returned NROUNDS * 3 and each store reached its own physical page. */
typedef unsigned long u64;
#define FINISHER ((volatile unsigned *)0x100000UL)
#define UART ((volatile unsigned char *)0x10000000UL)
#define NROUNDS 4
extern const unsigned char code_a[], code_a_end[], code_b[], code_b_end[];

static u64 root_a[512] __attribute__((aligned(4096)));
static u64 root_b[512] __attribute__((aligned(4096)));
static u64 l1_a[512]   __attribute__((aligned(4096)));
static u64 l1_b[512]   __attribute__((aligned(4096)));

#define PTE_V 1UL
#define PTE_RWX (2UL | 4UL | 8UL)
#define PTE_AD (64UL | 128UL)
static u64 leaf(u64 pa)  { return ((pa >> 12) << 10) | PTE_V | PTE_RWX | PTE_AD; }
static u64 table(u64 pa) { return ((pa >> 12) << 10) | PTE_V; }

static void put(const char *s) { while (*s) *UART = (unsigned char)*s++; }

static void copy(unsigned char *d, const unsigned char *s, const unsigned char *e) { while (s < e) *d++ = *s++; }

void s_main(void)
{
   const u64 satp_a = (8UL << 60) | ((u64)root_a >> 12), satp_b = (8UL << 60) | ((u64)root_b >> 12);
   u64 sum = 0;
   for (int r = 0; r < NROUNDS; r++)
   {
      asm volatile("csrw satp, %0; sfence.vma" :: "r"(satp_a) : "memory");
      sum += ((u64 (*)(void))0x40000000UL)();
      asm volatile("csrw satp, %0; sfence.vma" :: "r"(satp_b) : "memory");
      sum += ((u64 (*)(void))0x40000000UL)();
   }
   const int ok = sum == NROUNDS * 3
               && *(volatile unsigned *)0x802007f0UL == 0x11
               && *(volatile unsigned *)0x804007f0UL == 0x22;
   put(ok ? "rv-as ok\n" : "rv-as FAILED\n");
   *FINISHER = ok ? 0x5555 : 0x3333;
   for (;;) ;
}

void main_c(void)
{
   copy((unsigned char *)0x80200000UL, code_a, code_a_end);
   copy((unsigned char *)0x80400000UL, code_b, code_b_end);
   asm volatile("fence.i");
   for (int i = 0; i < 2; i++)
   {
      u64 *root = i ? root_b : root_a, *l1 = i ? l1_b : l1_a;
      root[0] = leaf(0x00000000UL);          /* devices, identity, 1 GiB */
      root[2] = leaf(0x80000000UL);          /* RAM, identity, 1 GiB */
      root[1] = table((u64)l1);              /* 0x40000000: 2 MiB pages */
      l1[0]   = leaf(i ? 0x80400000UL : 0x80200000UL);
   }
   /* PMP: everything, read/write/execute, for S-mode */
   asm volatile("csrw pmpaddr0, %0; csrw pmpcfg0, %1" :: "r"(-1UL), "r"(0x1fUL));
   /* mret into S-mode at s_main */
   u64 ms;
   asm volatile("csrr %0, mstatus" : "=r"(ms));
   ms = (ms & ~(3UL << 11)) | (1UL << 11);
   asm volatile("csrw mstatus, %0; csrw mepc, %1; mret" :: "r"(ms), "r"((u64)s_main));
   for (;;) ;
}

asm(".section .boot,\"ax\",@progbits\n"
    ".globl _start\n"
    "_start:\n"
    "  la sp, rv_stack\n"
    "  li t0, 16384\n"
    "  add sp, sp, t0\n"
    "  j main_c\n"
    ".text\n");
char rv_stack[16384] __attribute__((aligned(16)));
