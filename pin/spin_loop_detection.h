#ifndef __SPIN_LOOP_DETECTION_H
#define __SPIN_LOOP_DETECTION_H

#include "spin_loop_detector.h"
#include "inst_mode.h"

#include "pin.H"

namespace std
{
   template <> struct hash<REG> {
      size_t operator()(const REG & reg) const {
         return (int)reg;
      }
   };
}

struct SpinLoopDetectionState
{
   intptr_t write_addr;
   uint64_t write_value;
   ADDRINT reg_value[REG_MACHINE_LAST];
   SpinLoopDetector* sld;
};

void addSpinLoopDetection(TRACE trace, INS ins, InstMode::inst_mode_t inst_mode);

#endif // __SPIN_LOOP_DETECTION_H
