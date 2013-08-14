#ifndef __CPUID_H
#define __CPUID_H

typedef struct
{
   UInt32 eax, ebx, ecx, edx;
} cpuid_result_t;

static inline void cpuid(UInt32 eax, UInt32 ecx, cpuid_result_t &res)
{
    __asm__ __volatile__ (
      "cpuid\n"
      : "=a" (res.eax), "=b" (res.ebx), "=c" (res.ecx), "=d" (res.edx)
      : "a" (eax), "c" (ecx)
      );
}

#endif // __CPUID_H
