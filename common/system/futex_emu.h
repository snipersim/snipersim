// Define missing FUTEX flags when compiling on older kernels

#ifndef FUTEX_WAIT_BITSET
#  define FUTEX_WAIT_BITSET 9
#endif
#ifndef FUTEX_WAKE_BITSET
#  define FUTEX_WAKE_BITSET 10
#endif
#ifndef FUTEX_BITSET_MATCH_ANY
#  define FUTEX_BITSET_MATCH_ANY 0xffffffff
#endif

#ifndef FUTEX_PRIVATE_FLAG
// On older kernels that don't know about FUTEX_PRIVATE_FLAG: don't use it
// (Might be slightly slower but should still work)
#  define FUTEX_PRIVATE_FLAG 0
#endif

// Older kernels only cut out the flags they know about, we want just the lower bits
#undef FUTEX_CMD_MASK
#define FUTEX_CMD_MASK 0x7f
