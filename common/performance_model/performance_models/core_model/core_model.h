#ifndef __CORE_MODEL
#define __CORE_MODEL

#include "fixed_types.h"
#include "subsecond_time.h"
#include "allocator.h"
#include "dynamic_micro_op.h"

#include <map>

class Core;
class IntervalContention;
class RobContention;
class MicroOp;
class DynamicMicroOp;

class CoreModel
{
   private:
      static std::map<String, const CoreModel*> s_core_models;

   public:
      static const CoreModel* getCoreModel(String type);

      virtual IntervalContention* createIntervalContentionModel(const Core *core) const = 0;
      virtual unsigned int getLongLatencyCutoff() const = 0;

      // Return an Allocator for my type of DynamicMicroOp
      virtual Allocator* createDMOAllocator() const = 0;

      // Populate a MicroOp's core-specific information object
      virtual DynamicMicroOp* createDynamicMicroOp(Allocator *alloc, const MicroOp *uop, ComponentPeriod period) const = 0;

      virtual unsigned int getInstructionLatency(const MicroOp *uop) const = 0;
      virtual unsigned int getAluLatency(const MicroOp *uop) const = 0;
      virtual unsigned int getBypassLatency(const DynamicMicroOp *uop) const = 0;
      virtual unsigned int getLongestLatency() const = 0;
};

template <typename T> class BaseCoreModel : public CoreModel
{
   public:
      virtual Allocator* createDMOAllocator() const
      {
         // We need to be able to hold one (Pin) trace worth of MicroOps, as we can only stop functional simulation at the skew barrier
         return new TypedAllocator<T, 8192>();
      }

      DynamicMicroOp* createDynamicMicroOp(Allocator *alloc, const MicroOp *uop, ComponentPeriod period) const
      {
         T *info = DynamicMicroOp::alloc<T>(alloc, uop, this, period);
         return info;
      }
};

#endif // __CORE_MODEL
