// Define missing macros when compiling on older kernels

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

#ifndef CLOCK_MONOTONIC_RAW
#define CLOCK_MONOTONIC_RAW 4
#endif

#ifndef CLOCK_MONOTONIC_COARSE
#define CLOCK_MONOTONIC_COARSE 6
#endif

#ifndef CPU_SET_S
# define CPU_SET_S(cpu, setsize, cpusetp)   __CPU_SET_S (cpu, setsize, cpusetp)
# define CPU_CLR_S(cpu, setsize, cpusetp)   __CPU_CLR_S (cpu, setsize, cpusetp)
# define CPU_ISSET_S(cpu, setsize, cpusetp) __CPU_ISSET_S (cpu, setsize, \
                                                           cpusetp)
# define CPU_ZERO_S(setsize, cpusetp)       __CPU_ZERO_S (setsize, cpusetp)
# define CPU_COUNT_S(setsize, cpusetp)      __CPU_COUNT_S (setsize, cpusetp)

# define __CPU_SET_S(cpu, setsize, cpusetp) \
  (__extension__                                                              \
   ({ size_t __cpu = (cpu);                                                   \
      __cpu < 8 * (setsize)                                                   \
      ? (((__cpu_mask *) ((cpusetp)->__bits))[__CPUELT (__cpu)]               \
         |= __CPUMASK (__cpu))                                                \
      : 0; }))
# define __CPU_CLR_S(cpu, setsize, cpusetp) \
  (__extension__                                                              \
   ({ size_t __cpu = (cpu);                                                   \
      __cpu < 8 * (setsize)                                                   \
      ? (((__cpu_mask *) ((cpusetp)->__bits))[__CPUELT (__cpu)]               \
         &= ~__CPUMASK (__cpu))                                               \
      : 0; }))
# define __CPU_ISSET_S(cpu, setsize, cpusetp) \
  (__extension__                                                              \
   ({ size_t __cpu = (cpu);                                                   \
      __cpu < 8 * (setsize)                                                   \
      ? ((((__const __cpu_mask *) ((cpusetp)->__bits))[__CPUELT (__cpu)]      \
          & __CPUMASK (__cpu))) != 0                                          \
      : 0; }))

#  define __CPU_ZERO_S(setsize, cpusetp) \
  do __builtin_memset (cpusetp, '\0', setsize); while (0)
# define __CPU_COUNT_S(setsize, cpusetp) \
  __sched_cpucount (setsize, cpusetp)
#endif
