#ifndef DYNAMIC_INSTRUCTION_INFO_H
#define DYNAMIC_INSTRUCTION_INFO_H

#include "operand.h"
#include "mem_component.h"
#include "subsecond_time.h"

class HitWhere
{
   public:
      enum where_t
      {
         WHERE_FIRST = 0,
         L1I = MemComponent::L1_ICACHE,
         L1_OWN = MemComponent::L1_DCACHE,
         L2_OWN = MemComponent::L2_CACHE,
         L3_OWN = MemComponent::L3_CACHE,
         L4_OWN = MemComponent::L4_CACHE,
         MISS,
         DRAM_LOCAL,
         DRAM_REMOTE,
         CACHE_REMOTE,
         SIBLING,
         L1_SIBLING = MemComponent::L1_DCACHE + SIBLING,
         L2_SIBLING = MemComponent::L2_CACHE + SIBLING,
         L3_SIBLING = MemComponent::L3_CACHE + SIBLING,
         L4_SIBLING = MemComponent::L4_CACHE + SIBLING,
         UNKNOWN,
         PREDICATE_FALSE, // CMOV for which the predicate was false, did not actually execute
         NUM_HITWHERES
      };
};

const char * HitWhereString(HitWhere::where_t where);

struct DynamicInstructionInfo
{
   enum Type
   {
      MEMORY_READ,
      MEMORY_WRITE,
      STRING,
      BRANCH,
   } type;

   IntPtr eip;

   // Quick fix
   //union
   //{
      // MEMORY
      struct
      {
         bool executed; // For CMOV: true if executed
         SubsecondTime latency;
         IntPtr addr;
         UInt32 size;
         UInt32 num_misses;
         HitWhere::where_t hit_where;
      } memory_info;

      // STRING
      struct
      {
         UInt32 num_ops;
      } string_info;

      // BRANCH
      struct
      {
         bool taken;
         IntPtr target;
      } branch_info;
   //};

   // ctors

   DynamicInstructionInfo()
   {
   }

   DynamicInstructionInfo(const DynamicInstructionInfo &rhs)
   {
      type = rhs.type;
      eip = rhs.eip;
      memory_info = rhs.memory_info; // "use bigger one"
   }

   static DynamicInstructionInfo createMemoryInfo(IntPtr eip, bool e, SubsecondTime l, IntPtr a, UInt32 s, Operand::Direction dir, UInt32 num_misses, HitWhere::where_t hit_where)
   {
      DynamicInstructionInfo i;
      i.type = (dir == Operand::READ) ? MEMORY_READ : MEMORY_WRITE;
      i.eip = eip;
      i.memory_info.executed = e;
      i.memory_info.latency = l;
      i.memory_info.addr = a;
      i.memory_info.size = s;
      i.memory_info.num_misses = num_misses;
      i.memory_info.hit_where = hit_where;
      return i;
   }

   static DynamicInstructionInfo createStringInfo(IntPtr eip, UInt32 count)
   {
      DynamicInstructionInfo i;
      i.type = STRING;
      i.eip = eip;
      i.string_info.num_ops = count;
      return i;
   }

   static DynamicInstructionInfo createBranchInfo(IntPtr eip, bool taken, IntPtr target)
   {
      DynamicInstructionInfo i;
      i.type = BRANCH;
      i.eip = eip;
      i.branch_info.taken = taken;
      i.branch_info.target = target;
      return i;
   }
};

#endif
