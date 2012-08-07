#include "callstack.h"

struct stack_frame {
   struct stack_frame* next;
   void* ret;
};
extern void *__libc_stack_end;

int get_call_stack_from(void** retaddrs, int max_size, void* sp, void* bp)
{
   /* find the frame pointer */
   struct stack_frame* frame = (struct stack_frame*)bp;
   void* top_stack = sp;
   /* the rest just walks through the linked list */
   int i = 0;
   while(i < max_size)
   {
      if(frame < top_stack || frame > __libc_stack_end)
         break;
      retaddrs[i++] = frame->ret;
      frame = frame->next;
   }
   return i;
}

int get_call_stack(void** retaddrs, int max_size)
{
   int stack_top;
   return get_call_stack_from(retaddrs, max_size, &stack_top, __builtin_frame_address(0));
}
