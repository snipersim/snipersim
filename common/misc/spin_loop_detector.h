#ifndef __SPIN_LOOP_DETECTOR_H
#define __SPIN_LOOP_DETECTOR_H

#include "fixed_types.h"
#include "thread.h"

#include <unordered_map>
#include <deque>

class SpinLoopDetector
{
   private:
      static const size_t SDT_MAX_SIZE = 16;
      typedef uint16_t reg_t; // Pin's REG enum
      typedef std::pair<uint64_t, uint16_t> RubEntry;
      typedef std::unordered_map<reg_t, RubEntry> Rub;

      struct SdtEntry
      {
         SdtEntry(uint64_t _eip, uint8_t _id, uint64_t _icount, SubsecondTime _tcount) : eip(_eip), id(_id), icount(_icount), tcount(_tcount) {}
         uint64_t eip;
         uint8_t id;
         uint64_t icount;
         SubsecondTime tcount;
      };
      typedef std::deque<SdtEntry> Sdt;

      const Thread *m_thread;
      Rub m_rub; // Register Update Buffer
      Sdt m_sdt; // Spin Detection Table
      uint16_t m_sdt_bitmask; // Bitmask of all valid SDT entries
      uint8_t m_sdt_nextid;

   public:
      SpinLoopDetector(Thread *thread) : m_thread(thread), m_rub(), m_sdt(), m_sdt_bitmask(0), m_sdt_nextid(0) {}

      void commitBCT(uint64_t eip);
      void commitNonSilentStore();
      void commitRegisterWrite(reg_t reg, uint64_t old_value, uint64_t value);

      bool inCandidateSpin() { return !m_sdt.empty(); }
};

#endif // __SPIN_LOOP_DETECTOR_H
