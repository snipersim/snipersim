/* A RISC-V guest for the QEMU frontend's RISC-V cases: freestanding (no libc,
 * so any RISC-V clang or gcc builds it), a counted loop of 20,000 iterations
 * -- 100,007 instructions retired, with one conditional branch per iteration
 * -- and an exit ecall.  Built by testing/test_qemu.py when SNIPER_RISCV_CC
 * names a compiler:
 *   $CC --target=riscv64-linux-gnu -march=rv64gc -mabi=lp64d -O1 -nostdlib \
 *       -static -fuse-ld=lld -Wl,-e,_start -o rv-loop rv-loop.c            */
static long work(long n) { long s = 0; for (long i = 0; i < n; i++) s += i ^ (s >> 3); return s; }
static void exit_(long code) { register long a0 __asm__("a0") = code; register long a7 __asm__("a7") = 93; __asm__ volatile("ecall" : : "r"(a0), "r"(a7)); }
void _start(void) { exit_(work(20000) & 0x7f); }
