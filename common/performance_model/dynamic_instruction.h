#ifndef __DYNAMIC_INSTRUCTION_H
#define __DYNAMIC_INSTRUCTION_H

#include "operand.h"
#include "subsecond_time.h"
#include "hit_where.h"
#include "allocator.h"

class Core;
class Instruction;

class DynamicInstruction
{
   private:
      // Private constructor: alloc() should be used
      DynamicInstruction(Instruction *ins, IntPtr _eip)
      {
         instruction = ins;
         eip = _eip;
         branch_info.is_branch = false;
         num_memory = 0;
      }
   public:
      struct BranchInfo
      {
         bool is_branch;
         bool taken;
         IntPtr target;
      };
      struct MemoryInfo {
         bool executed; // For CMOV: true if executed
         Operand::Direction dir;
         IntPtr addr;
         UInt32 size;
         UInt32 num_misses;
         SubsecondTime latency;
         HitWhere::where_t hit_where;
      };
      static const UInt8 MAX_MEMORY = 2;

      Instruction* instruction;
      IntPtr eip; // Can be physical address, so different from instruction->getAddress() which is always virtual
      BranchInfo branch_info;
      UInt8 num_memory;
      MemoryInfo memory_info[MAX_MEMORY];

      static Allocator* createAllocator();

      ~DynamicInstruction();

      static DynamicInstruction* alloc(Allocator *alloc, Instruction *ins, IntPtr eip)
      {
         void *ptr = alloc->alloc(sizeof(DynamicInstruction));
         DynamicInstruction *i = new(ptr) DynamicInstruction(ins, eip);
         return i;
      }
      static void operator delete(void* ptr) { Allocator::dealloc(ptr); }

      SubsecondTime getCost(Core *core);

      bool isBranch() const { return branch_info.is_branch; }
      bool isMemory() const { return num_memory > 0; }

      void addMemory(bool e, SubsecondTime l, IntPtr a, UInt32 s, Operand::Direction dir, UInt32 num_misses, HitWhere::where_t hit_where)
      {
         LOG_ASSERT_ERROR(num_memory < MAX_MEMORY, "Got more than MAX_MEMORY(%d) memory operands", MAX_MEMORY);
         memory_info[num_memory].dir = dir;
         memory_info[num_memory].executed = e;
         memory_info[num_memory].latency = l;
         memory_info[num_memory].addr = a;
         memory_info[num_memory].size = s;
         memory_info[num_memory].num_misses = num_misses;
         memory_info[num_memory].hit_where = hit_where;
         num_memory++;
      }

      void addBranch(bool taken, IntPtr target)
      {
         branch_info.is_branch = true;
         branch_info.taken = taken;
         branch_info.target = target;
      }

      SubsecondTime getBranchCost(Core *core, bool *p_is_mispredict = NULL);
      void accessMemory(Core *core);
};

#endif // __DYNAMIC_INSTRUCTION_H
