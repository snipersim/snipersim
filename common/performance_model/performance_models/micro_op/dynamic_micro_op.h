#ifndef __DYNAMIC_MICRO_OP_INFO_H
#define __DYNAMIC_MICRO_OP_INFO_H

#include "fixed_types.h"
#include "allocator.h"
#include "subsecond_time.h"
#include "memory_access.h"
#include "micro_op.h"
#include "hit_where.h"
#include "allocator.h"

class CoreModel;

class DynamicMicroOp
{
   private:
      const MicroOp *m_uop;
      const CoreModel *m_core_model;

      // architecture-independent information
      const SubsecondTime m_period;

      /** The sequence number of the microOperation. Unique (per thread) ! */
      uint64_t sequenceNumber;

      /** The address is valid for UOP_LOAD and UOP_STORE, it contains the load or store address. */
      Memory::Access address;

      /** The microop has been squashed */
      bool squashed;

      /** Initially copied from MicroOp, but can be changed by removeDependency */
      uint32_t intraInstructionDependencies;
      /** Initially copied from MicroOp, but can be changed in case of squashing */
      uint32_t microOpTypeOffset;
      /** Used in doSquashing. Contains number of squashed preceding uops of the instruction*/
      uint32_t squashedCount;
      /** This field contains the length of the dependencies array. */
      uint32_t dependenciesLength;
      /** This array contains the dependencies. The uint64_t stored in the array is the sequenceNumber of the dependency. */
      uint64_t dependencies[MAXIMUM_NUMBER_OF_DEPENDENCIES];

      /** The latency of the instruction. */
      uint32_t execLatency;

      /** Did a jump occur after this instruction ? */
      bool branchTaken;
      /** Is branch mispredicted ? Only for UOP_EXECUTE and branches. */
      bool branchMispredicted;
      /** Branch target address */
      IntPtr branchTargetAddress;

      HitWhere::where_t dCacheHitWhere;
      HitWhere::where_t iCacheHitWhere;
      uint32_t iCacheLatency;

      bool m_forceLongLatencyLoad;

      /** These first/last flags are needed in case of squashing, as long as squashed uop doesn't go to rob
          and we can't use first/last flags of m_uop in such a cases.*/
      /** This microOp is the first microOp of the instruction. */
      bool first;
      /** This microOp is the last microOp of the instruction. */
      bool last;

      // architecture-specific information to be defined in derived classes


   public:

      DynamicMicroOp(const MicroOp *uop, const CoreModel *core_model, ComponentPeriod period);
      virtual ~DynamicMicroOp();

      template<typename T> static T* alloc(Allocator *alloc, const MicroOp *uop, const CoreModel *core_model, ComponentPeriod period)
      {
         void *ptr = alloc->alloc(sizeof(T));
         T *t = new(ptr) T(uop, core_model, period);
         return t;
      }
      static void operator delete(void* ptr) { Allocator::dealloc(ptr); }

      const MicroOp *getMicroOp() const { return m_uop; }

      template<typename T> const T* getCoreSpecificInfo() const {
         const T *ptr = dynamic_cast<const T*>(this);
         LOG_ASSERT_ERROR(ptr != NULL, "DynamicMicroOp of the wrong type: expected %s, got %s", /*T::getType()*/"???", this->getType());
         return ptr;
      }

      // Mark this micro-op as squashed so it will be ignored in further pipeline stages
      void squash(std::vector<DynamicMicroOp*>* array = NULL);
      bool isSquashed() { return squashed; }

      uint32_t getDependenciesLength() const { return this->intraInstructionDependencies + this->dependenciesLength; }
      uint64_t getDependency(uint32_t index) const;
      void addDependency(uint64_t sequenceNumber);
      void removeDependency(uint64_t sequenceNumber);

      uint32_t getIntraInstrDependenciesLength() const { return this->intraInstructionDependencies; }
      void setIntraInstrDependenciesLength(uint32_t deps) { intraInstructionDependencies = deps;}

      uint32_t getMicroOpTypeOffset() const { return microOpTypeOffset; }
      void setMicroOpTypeOffset(uint32_t offset) { microOpTypeOffset = offset; }

      uint32_t getSquashedCount() const { return squashedCount; }
      void setSquashedCount(uint32_t count) { squashedCount = count; }

      void setFirst(bool _first) { first = _first; }
      bool isFirst() const { return first; }

      void setLast(bool _last) { last = _last; }
      bool isLast() const { return last; }

      bool isBranchTaken() const { LOG_ASSERT_ERROR(m_uop->isBranch(), "Expected a branch instruction."); return this->branchTaken; }
      void setBranchTaken(bool _branch_taken) { LOG_ASSERT_ERROR(m_uop->isBranch(), "Expected a branch instruction."); branchTaken = _branch_taken; }
      bool isBranchMispredicted() const { LOG_ASSERT_ERROR(m_uop->isBranch(), "Expected a branch instruction."); return this->branchMispredicted; }
      void setBranchMispredicted(bool mispredicted) { this->branchMispredicted = mispredicted; }
      IntPtr getBranchTarget() const { LOG_ASSERT_ERROR(m_uop->isBranch(), "Expected a branch instruction."); return this->branchTargetAddress; }
      void setBranchTarget(IntPtr address) { this->branchTargetAddress = address; }

      const Memory::Access& getLoadAccess() const;
      bool isLongLatencyLoad() const;
      const Memory::Access& getStoreAccess() const;

      uint32_t getExecLatency() const { return this->execLatency; }
      void setExecLatency(uint32_t latency) { this->execLatency = latency; }

      uint64_t getSequenceNumber() const { return this->sequenceNumber; }
      void setSequenceNumber(uint64_t number) { this->sequenceNumber = number; }

      HitWhere::where_t getDCacheHitWhere() const { return dCacheHitWhere; }
      void setDCacheHitWhere(HitWhere::where_t _hitWhere) { dCacheHitWhere = _hitWhere; }

      HitWhere::where_t getICacheHitWhere() const { return iCacheHitWhere; }
      void setICacheHitWhere(HitWhere::where_t _hitWhere) { iCacheHitWhere = _hitWhere; }

      uint32_t getICacheLatency() const { return iCacheLatency; }
      void setICacheLatency(uint32_t _latency) { iCacheLatency = _latency; };


      void setAddress(const Memory::Access& loadAccess) { this->address = loadAccess; }
      const Memory::Access& getAddress(void) const { return this->address; }

      void setForceLongLatencyLoad(bool forceLLL) { m_forceLongLatencyLoad = forceLLL; }

      SubsecondTime getPeriod() const { LOG_ASSERT_ERROR(m_period != SubsecondTime::Zero(), "MicroOp Period is == SubsecondTime::Zero()"); return m_period; }


      // More dynamic, architecture-dependent information to be defined by derived classes
      virtual const char* getType() const = 0; // Make this class pure virtual
};

#endif // __DYNAMIC_MICRO_OP_INFO_H
