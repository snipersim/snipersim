#ifndef __SIFT_H
#define __SIFT_H

// Sniper Instruction Trace File Format
//
// x86-64 only, little-endian

#include <stdint.h>

extern "C" {
#include "xed-interface.h"
}

namespace Sift
{
   const uint32_t MAX_DYNAMIC_ADDRESSES = 2;
};

#endif // __SIFT_H
