#ifndef __RNG_H
#define __RNG_H

// RNG parameters, defaults taken from drand48
#define RNG_A __UINT64_C(0x5DEECE66D)
#define RNG_C __UINT64_C(0xB)
#define RNG_M ((__UINT64_C(1) << 48) - 1)

// Same as drand48, but inlined for efficiency
inline UInt64 rng_next(UInt64 &state)
{
   state = (RNG_A * state + RNG_C) & RNG_M;
   return state >> 16;
}
inline UInt64 rng_seed(UInt64 seed)
{
   return (seed << 16) + 0x330E;
}

#endif // __RNG_H
