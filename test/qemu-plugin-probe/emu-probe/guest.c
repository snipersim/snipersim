/* Executes an instruction QEMU does not know (ame.release's provisional word)
 * with a1 = 7 and a2 = &slot; exits with a0 + (slot == marker ? 100 : 0).
 * Without emulation: SIGILL.  With the plugin: exit code 42 + 100 = 142. */
static unsigned long slot;
void _start(void) {
   register long a0 __asm__("a0") = 0; register long a1 __asm__("a1") = 7; register unsigned long *a2 __asm__("a2") = &slot;
   __asm__ volatile(".word 0xa4a0002b" : "+r"(a0) : "r"(a1), "r"(a2) : "memory");
   long code = a0 + (slot == 0x1234567890abcdefULL ? 100 : 0);
   register long a7 __asm__("a7") = 94; register long x0 __asm__("a0") = code;
   __asm__ volatile("ecall" : : "r"(a7), "r"(x0)); for (;;) ;
}
