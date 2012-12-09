#ifndef __SPIN_LOOP_DETECTOR_H
#define __SPIN_LOOP_DETECTOR_H

#include "fixed_types.h"

#include <unordered_map>
#include <deque>

class SpinLoopDetector
{
   private:
      static const size_t SDT_MAX_SIZE = 16;
      typedef uint16_t reg_t; // Pin's REG enum
      typedef std::pair<uint64_t, uint16_t> RubEntry;
      typedef std::unordered_map<reg_t, RubEntry> Rub;
      typedef std::pair<uint64_t, uint8_t> SdtEntry;
      typedef std::deque<SdtEntry> Sdt;

      Rub m_rub; // Register Update Buffer
      Sdt m_sdt; // Spin Detection Table
      uint16_t m_sdt_bitmask; // Bitmask of all valid SDT entries
      uint8_t m_sdt_nextid;

   public:
      SpinLoopDetector() : m_rub(), m_sdt(), m_sdt_bitmask(0), m_sdt_nextid(0) {}

      void commitBCT(uint64_t eip);
      void commitNonSilentStore();
      void commitRegisterWrite(reg_t reg, uint64_t old_value, uint64_t value);

      bool inCandidateSpin() { return !m_sdt.empty(); }
};

#endif // __SPIN_LOOP_DETECTOR_H
