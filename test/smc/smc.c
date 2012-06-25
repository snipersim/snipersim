/*
 * A self-modifying code snippet that uses C
 * and the GCC label to pointer (&&) extension.
 *
 * Based on http://nikmav.blogspot.be/2011/12/self-modifying-code-using-gcc.html
 */

#include <stdio.h>
#include <string.h>
#include <stdlib.h>
#include <unistd.h>
#include <sys/mman.h>
#include <stdint.h>

int main(int argc, char **argv)
{
   int (*my_printf) (const char *format, ...);
   void (*my_exit) (int);
   void *page =
      (void *) ((unsigned long) (&&checkpoint) &
         ~(getpagesize() - 1));

   /* mark the code section we are going to overwrite
    * as writable.
    */
   mprotect(page, getpagesize(), PROT_READ | PROT_WRITE | PROT_EXEC);

   /* Use the labels to avoid having GCC
    * optimize them out */
   switch (argc) {
      case 33:
         goto checkpoint;
      case 44:
         goto newcode;
      case 55:
         goto newcode_end;
      default:
         break;
   }

checkpoint:
   printf("Good morning!\n");

   /* Replace code in checkpoint with code from
    * newcode.
    */
   memcpy(&&checkpoint, &&newcode, &&newcode_end - &&newcode);

   /* Re-execute checkpoint, now with replaced code */
   goto checkpoint;

newcode:
   my_printf = &printf;
   (*(my_printf)) ("Good evening\n");

   my_exit = &exit;
   (*(my_exit)) (0);

newcode_end:
   return 2;
}
