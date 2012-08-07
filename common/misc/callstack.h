#ifndef __CALLSTACK_H
#define __CALLSTACK_H

int get_call_stack(void** retaddrs, int max_size);
int get_call_stack_from(void** retaddrs, int max_size, void* sp, void* bp);

#endif // __CALLSTACK_H
