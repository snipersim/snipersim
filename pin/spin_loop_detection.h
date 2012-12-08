#ifndef __SPIN_LOOP_DETECTION_H
#define __SPIN_LOOP_DETECTION_H

#include "pin.H"
#include <unordered_map>
#include <deque>

namespace std
{
   template <> struct hash<REG> {
      size_t operator()(const REG & reg) const {
         return (int)reg;
      }
   };
}

#define SDT_MAX_SIZE 16

struct SpinLoopDetectionState
{
   intptr_t write_addr;
   uint64_t write_value;
   ADDRINT reg_value[REG_MACHINE_LAST];
   std::unordered_map<REG, std::pair<ADDRINT, uint16_t> > rub; // Register Update Buffer
   std::deque<std::pair<ADDRINT, uint8_t> > sdt; // Spin Detection Table
   uint16_t sdt_bitmask; // Bitmask of all valid SDT entries
   uint8_t sdt_nextid;
};

void addSpinLoopDetection(TRACE trace, INS ins);

#endif // __SPIN_LOOP_DETECTION_H
