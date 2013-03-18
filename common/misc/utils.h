#ifndef UTILS_H
#define UTILS_H

#include "fixed_types.h"
#include <assert.h>
#include <sstream>
#include <iostream>


String myDecStr(UInt64 v, UInt32 w);


#define safeFDiv(x) (x ? (double) x : 1.0)


// Checks if n is a power of 2.
// returns true if n is power of 2

bool isPower2(UInt32 n);


// Computes floor(log2(n))
// Works by finding position of MSB set.
// returns -1 if n == 0.

SInt32 floorLog2(UInt32 n);


// Computes floor(log2(n))
// Works by finding position of MSB set.
// returns -1 if n == 0.

SInt32 ceilLog2(UInt32 n);

// Max and Min functions
template <class T>
T getMin(T v1, T v2)
{
   return (v1 < v2) ? v1 : v2;
}

template <class T>
T getMax(T v1, T v2)
{
   return (v1 > v2) ? v1 : v2;
}

#endif

UInt64 countBits(UInt64 n);
