#ifndef MAGIC_SERVER_H
#define MAGIC_SERVER_H

#include "fixed_types.h"
#include "progress.h"

#include <unordered_set>
#include <mutex>
#include <atomic>

class MagicServer
{
   public:
      // data type to hold arguments in a HOOK_MAGIC_MARKER callback
      struct MagicMarkerType {
         thread_id_t thread_id;
         core_id_t core_id;
         UInt64 arg0, arg1;
         const char* str;
      };

      MagicServer();
      ~MagicServer();

      /* respond, when given, is cleared by the commands whose caller must not
         be answered now: the MimicOS context switches, after which another
         stream runs and the caller is answered when it is switched back to.
         Every other command leaves it set, whatever its answer. */
      UInt64 Magic(thread_id_t thread_id, core_id_t core_id, UInt64 cmd, UInt64 arg0, UInt64 arg1, bool *respond = nullptr);
      bool inROI(void) const { return m_performance_enabled; }
      static UInt64 getGlobalInstructionCount(void);

      // To be called while holding the thread manager lock
      UInt64 Magic_unlocked(thread_id_t thread_id, core_id_t core_id, UInt64 cmd, UInt64 arg0, UInt64 arg1, bool *respond = nullptr);
      UInt64 setFrequency(UInt64 core_number, UInt64 freq_in_mhz);
      UInt64 getFrequency(UInt64 core_number);

      void enablePerformance();
      void disablePerformance();
      UInt64 setPerformance(bool enabled);

      /* Per-thread ROI barrier (Apr 15 2026). */
      UInt64 roiExpect(UInt64 n);                     // set expected joiner count
      UInt64 threadRoiJoin(thread_id_t thread_id);    // thread enters ROI
      UInt64 threadRoiLeave(thread_id_t thread_id);   // thread leaves ROI
      UInt64 waitForRoi();                            // block until ROI is ready
      bool roiReady() const                           // non-blocking form
         { return m_roi_ready.load(std::memory_order_acquire); }

      UInt64 setInstrumentationMode(UInt64 sim_api_opt);

      void setProgress(float progress) { m_progress.setProgress(progress); }

   private:
      bool m_performance_enabled;
      Progress m_progress;

      /* Per-thread ROI barrier state. */
      std::mutex m_roi_lock;
      size_t m_roi_expected = 0;
      std::unordered_set<thread_id_t> m_roi_joined;
      bool m_legacy_roi_active = false;      // true if SimRoiStart was called
      std::atomic<bool> m_roi_ready{false};  // set when perf model enables
};

#endif // SYNC_SERVER_H
