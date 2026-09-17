#ifndef __SIFT_H
#define __SIFT_H

// Sniper Instruction Trace File Format
//
// x86-64 only, little-endian

#include <cstdint>
#include <cstddef>

namespace Sift
{
   const uint32_t MAX_DYNAMIC_ADDRESSES = 2;
   // An instruction that touches more memory than MAX_DYNAMIC_ADDRESSES can
   // name (vector and matrix-tile accesses) describes its footprint as up to
   // MAX_MEMORY_SPANS contiguous byte ranges (Writer::MemorySpans), recorded
   // ahead of the instruction and delivered with it (Instruction::spans).
   const uint32_t MAX_MEMORY_SPANS = 16;
   typedef struct
   {
      uint64_t addr;
      uint32_t size;
      uint8_t is_write;
   } __attribute__ ((__packed__)) MemorySpan;

   typedef enum
   {
      MemInvalidLock = 0,
      MemNoLock,
      MemLock,
      MemUnlock,
   } MemoryLockType;

   typedef enum
   {
      MemInvalidOp = 0,
      MemRead,
      MemWrite,
   } MemoryOpType;

   typedef enum
   {
      RoutineEnter = 0,
      RoutineExit,
      RoutineAssert,
   } RoutineOpType;
};

#endif // __SIFT_H
