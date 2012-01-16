#ifndef __LLL_INFO_H
#define __LLL_INFO_H

#include "simulator.h"
#include "log.h"
#include "lock.h"
#include "config.hpp"

#define LLL_CUTOFF_DEFAULT 30 // 0 is disabled
//#define LLL_CUTOFF_DEFAULT (1+3+31) // FIXME Dunnington specific: when bigger than (L1 tags + L2 tags + L3 Data), we must be a LLL

// Long Latency Load configuration information
// Defined on a cache-line to prevent false-sharing with other structures
class LLLInfo {
public:
   LLLInfo()
   {}
   // This should only be called in a constructor for Pin-based simulations
   // The reason is that the getCfg()->get*() functions use floating point, which shouldn't be used in pin simulations
   static void initialize()
   {
      ScopedLock lll_lock(m_lll_lock);
      m_cutoff = Sim()->getCfg()->getInt("perf_model/core/interval_timer/lll_cutoff", LLL_CUTOFF_DEFAULT);
      m_initialized = true;
   }
   uint32_t getCutoff()
   {
      // Report error if we haven't been initialized yet, as we need to initialize this value before starting our simulation
      if (!m_initialized)
      {
         LOG_PRINT_ERROR("LLLInfo has not been correctly initialized!  It should not be used before initialization");
      }
      return m_cutoff;
   }
private:
   static uint32_t m_cutoff;
   static bool m_initialized;
   static Lock m_lll_lock;
};

// Reuse LLLInfo defined in micro_op.cc
extern LLLInfo lll_info;

#endif
