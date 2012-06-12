#ifndef CONTENTION_MODEL_H
#define CONTENTION_MODEL_H

#include <vector>
#include "fixed_types.h"
#include "subsecond_time.h"

class ContentionModel {
   private:
      UInt32 m_num_outstanding;
      std::vector<std::pair<SubsecondTime, UInt64> > m_time;
      SubsecondTime m_t_last;
      const ComponentPeriod *m_proc_period;
   public:
      UInt64 m_n_requests;
      UInt64 m_n_barriers;
      UInt64 m_n_outoforder;
      UInt64 m_n_simultaneous;
      UInt64 m_n_hasfreefail;
      SubsecondTime m_total_delay;
      SubsecondTime m_total_barrier_delay;

      ContentionModel();
      ContentionModel(String name, core_id_t core_id, UInt32 num_outstanding = 1);
      ~ContentionModel();

      uint64_t getBarrierCompletionTime(uint64_t t_start, uint64_t t_delay, UInt64 tag = 0); // Support legacy components
      SubsecondTime getBarrierCompletionTime(SubsecondTime t_start, SubsecondTime t_delay, UInt64 tag = 0);
      uint64_t getCompletionTime(uint64_t t_start, uint64_t t_delay, UInt64 tag = 0); // Support legacy components
      SubsecondTime getCompletionTime(SubsecondTime t_start, SubsecondTime t_delay, UInt64 tag = 0);
      uint64_t getStartTime(uint64_t t_start);
      SubsecondTime getStartTime(SubsecondTime t_start);

      UInt32 getNumUsed(uint64_t t_start);
      UInt32 getNumUsed(SubsecondTime t_start);
      SubsecondTime getTagCompletionTime(UInt64 tag);
      bool hasFreeSlot(SubsecondTime t_start, UInt64 tag = -1);
      bool hasFreeSlot(uint64_t t_start, UInt64 tag = -1);
      bool hasTag(UInt64 tag);
};

#endif // CONTENTION_MODEL_H
