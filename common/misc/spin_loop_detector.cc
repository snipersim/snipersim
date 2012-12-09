// Implements spin loop detection as described by
// T. Li, A. R. Lebeck, and D. J. Sorin.
// "Spin detection hardware for improved management of multithreaded systems."
// IEEE Transactions on Parallel and Distributed Systems (TPDS), 17:508-521, June 2006.

#include "spin_loop_detector.h"
#include "core.h"
#include "performance_model.h"

void SpinLoopDetector::commitBCT(uint64_t eip)
{
   // We just executed a backward control transfer (BCT)
   // ``A BCT can be a backward conditional taken branch, unconditional branch, or jump instruction.''

   // Note: since we keep the SDT as a queue, a BCT's position is not constant so we use explicit ids
   // We also have an unbounded RUB so there is no overflow bit

   Core *core = m_thread->getCore();

   // Find BCT in the SDT
   Sdt::iterator entry = m_sdt.begin();
   for( ; entry != m_sdt.end(); ++entry)
      if (entry->eip == eip)
         break;

   if (entry == m_sdt.end())
   {
      // Unknown BCT: add to SDT, increment next SDT entry id, and update mask of valid ids
      m_sdt.push_back(SdtEntry(eip, m_sdt_nextid, core->getInstructionCount(), core->getPerformanceModel()->getElapsedTime()));
      m_sdt_nextid = (m_sdt_nextid + 1) % SDT_MAX_SIZE;

      // Keep only top SDT_MAX_SIZE entries
      if (m_sdt.size() > SDT_MAX_SIZE)
         m_sdt.pop_front();

      m_sdt_bitmask = 0;
      for(Sdt::iterator it = m_sdt.begin(); it != m_sdt.end(); ++it)
         m_sdt_bitmask |= (1 << it->id);
   }
   else
   {
      // Found! Check all RUB entries for our id to check if observable register state
      // has changed since the end of last iteration (Spin Condition 1)
      uint8_t id = entry->id;

      bool spinning = true;
      for(Rub::iterator it = m_rub.begin(); it != m_rub.end(); ++it)
      {
         if (it->second.second & (1 << id))
         {
            // ``There exists at least one entry in the RUB that corresponds to this SDT entry.
            //   This means that the observable register state of the thread differs and, thus,
            //   the thread was not spinning.''
            spinning = false;
            // Reset RUB entry for our loop id
            it->second.second &= ~(1 << id);
         }
      }

      if (spinning)
      {
         // Spin loop detected !!
         core->updateSpinCount(
            core->getInstructionCount() - entry->icount,
            // This time difference will have been caused by instructions earlier in the queue, not by the instructions
            // in this exact spin loop. But, when spinning for a long time, the earlier instructions will be spin loops as well
            // so if spin count is large (which is when we care) it should be reasonably accurate.
            core->getPerformanceModel()->getElapsedTime() - entry->tcount
         );
      }

      entry->icount = core->getInstructionCount();
      entry->tcount = core->getPerformanceModel()->getElapsedTime();
   }
}

void SpinLoopDetector::commitNonSilentStore()
{
   // ``Whenever the processor commits a non-silent store, it clears the SDT.''
   m_sdt.clear();
   m_rub.clear();
}

void SpinLoopDetector::commitRegisterWrite(reg_t reg, uint64_t old_value, uint64_t value)
{
   // Implements Register Update Buffer (RUB), see Section 3.2, 3rd paragraph

   // ``For each instruction it then commits, the processor checks if the instruction's architectural
   //   (i.e., logical) destination register is already in the RUB.''
   if (m_rub.count(reg) == 0)
   {
      // ``If not, the processor has discovered a new register written by the thread
      //   It then compares the new value of this register (i.e., the value being committed)
      //   to its old value (i.e., the value before being overwritten by the commit).''
      if (value != old_value)
      {
         // ``If not equal, the processor adds the register number and its old value to the RUB.''
         m_rub[reg] = std::make_pair(old_value, m_sdt_bitmask);
      }
   }
   else
   {
      // ``If the register is already in the RUB, then the processor compares its new value to its current value in the RUB.''
      if (value == m_rub[reg].first)
      {
         // ``If equal, it deletes this register from the RUB;''
         m_rub.erase(reg);
      }
      // ``otherwise, no action is necessary.''
      else
      {
         // Actually, this happens when the register is already in the RUB for an older loop.
         // Unless it's a silent write, update the bitmask to represent all current loop candidates.
         if (value != old_value)
            m_rub[reg].second |= m_sdt_bitmask;
      }
   }
}
