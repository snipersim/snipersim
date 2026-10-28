#ifndef __CPUID_H
#define __CPUID_H

typedef struct
{
   UInt32 eax, ebx, ecx, edx;
} cpuid_result_t;

/* The host's CPUID, which Core::emulateCpuid passes on to an x86 guest
   (adjusted for the simulated core).  On a host that is not x86 there is none
   to pass on: the guest sees zeros, no features; and the x86 instruction no
   longer keeps the simulator from building there. */
static inline void cpuid(UInt32 eax, UInt32 ecx, cpuid_result_t &res)
{
#if defined(__x86_64__) || defined(__i386__)
    __asm__ __volatile__ (
      "cpuid\n"
      : "=a" (res.eax), "=b" (res.ebx), "=c" (res.ecx), "=d" (res.edx)
      : "a" (eax), "c" (ecx)
      );
#else
    (void)eax; (void)ecx;
    res.eax = res.ebx = res.ecx = res.edx = 0;
#endif
}

#endif // __CPUID_H
