// Define missing FUTEX flags when compiling on older kernels

#ifndef FUTEX_WAIT_BITSET
#  define FUTEX_WAIT_BITSET 9
#endif
#ifndef FUTEX_WAKE_BITSET
#  define FUTEX_WAKE_BITSET 10
#endif

// Older kernels only cut out the flags they know about, we want just the lower bits
#undef FUTEX_CMD_MASK
#define FUTEX_CMD_MASK 0x7f
