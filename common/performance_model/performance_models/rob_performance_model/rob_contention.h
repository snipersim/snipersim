/*
 * This file is covered under the Interval Academic License, see LICENCE.interval
 */

#ifndef __ROB_CONTENTION_H
#define __ROB_CONTENTION_H

#include "micro_op.h"

class Core;
class CoreModel;
class DynamicMicroOp;

class RobContention {
   public:
      static RobContention* createRobContentionModel(Core *core, const CoreModel *core_model);

      virtual void initCycle(SubsecondTime now) = 0;
      virtual bool tryIssue(const DynamicMicroOp &uop) = 0;
      virtual bool noMore() { return false; } // Optimization: all resources used, nothing more can be issued this cycle
      virtual void doIssue(DynamicMicroOp &uop) = 0;
};

#endif // __ROB_CONTENTION_H
