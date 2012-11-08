#ifndef __FASTFORWARD_PERFORMANCE_MANAGER_H
#define __FASTFORWARD_PERFORMANCE_MANAGER_H

#include "fixed_types.h"
#include "subsecond_time.h"

class FastForwardPerformanceManager
{
   public:
      static FastForwardPerformanceManager* create();

      FastForwardPerformanceManager();
      void enable();
      void disable();

   protected:
      friend class FastforwardPerformanceModel;

      void recalibrateInstructionsCallback(core_id_t core_id);

   private:
      const SubsecondTime m_sync_interval;
      bool m_enabled;
      SubsecondTime m_target_sync_time;

      static SInt64 hook_instr_count(UInt64 self, UInt64 core_id) { ((FastForwardPerformanceManager*)self)->instr_count(core_id); return 0; }
      static SInt64 hook_periodic(UInt64 self, UInt64 time) { ((FastForwardPerformanceManager*)self)->periodic(*(subsecond_time_t*)&time); return 0; }

      void instr_count(core_id_t core_id);
      void periodic(SubsecondTime time);
      void step();
};

#endif // __FASTFORWARD_PERFORMANCE_MANAGER_H
