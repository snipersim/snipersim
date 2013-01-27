/*
 * This file is covered under the Interval Academic License, see LICENCE.interval
 */

#ifndef __INTERVAL_CONTENTION_H
#define __INTERVAL_CONTENTION_H

#include "fixed_types.h"

class Core;
class CoreModel;
class DynamicMicroOp;

class IntervalContention {
   public:
      static IntervalContention* createIntervalContentionModel(Core *core, const CoreModel *core_model);

      virtual void clearFunctionalUnitStats() = 0;
      virtual void addFunctionalUnitStats(const DynamicMicroOp *uop) = 0;
      virtual void removeFunctionalUnitStats(const DynamicMicroOp *uop) = 0;
      virtual uint64_t getEffectiveCriticalPathLength(uint64_t critical_path_length, bool update_reason) = 0;
};

#endif // __INTERVAL_CONTENTION_H
