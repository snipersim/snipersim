/* Code rewritten in place, as a JIT or a loader reusing a page does it.
 *
 * A fresh page gets code A ("mov eax, 1; ret"), which runs; then code B, whose
 * first instruction is a different length at the same address ("xor eax, eax;
 * inc eax; inc eax; ret"), which runs; then code A again.  Natively every run
 * returns its own value.  Under Sniper the trace must carry B's bytes for the
 * second run, and the simulator must decode them rather than reuse A's: with
 * a stale decode the 5-byte A instruction is timed where B's 2-byte one ran.
 *
 * Reported for RISC-V with the old Spike front end (issue: "SIFT Reader fails
 * to change the instruction it has associated with the PC"); the same caching
 * affected every front end. */
#include <stdio.h>
#include <string.h>
#include <sys/mman.h>

static const unsigned char code_a[] = { 0xb8, 0x01, 0x00, 0x00, 0x00, 0xc3 };           /* mov eax, 1; ret */
static const unsigned char code_b[] = { 0x31, 0xc0, 0xff, 0xc0, 0xff, 0xc0, 0xc3 };     /* xor eax, eax; inc eax; inc eax; ret */

static void run(const char *tag, unsigned char *page, const unsigned char *code, size_t n, int expected)
{
   memcpy(page, code, n);
   int r = ((int (*)(void))page)();
   printf("%s pc=%p returned=%d expected=%d\n", tag, (void *)page, r, expected);
   fflush(stdout);
}

int main(void)
{
   unsigned char *page = mmap(0, 4096, PROT_READ | PROT_WRITE | PROT_EXEC, MAP_PRIVATE | MAP_ANONYMOUS, -1, 0);
   run("A first", page, code_a, sizeof(code_a), 1);
   run("B rewritten", page, code_b, sizeof(code_b), 2);
   run("A again", page, code_a, sizeof(code_a), 1);
   return 0;
}
