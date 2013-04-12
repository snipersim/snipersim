#ifndef __PERIODIC_SAMPLING
#define __PERIODIC_SAMPLING

#include "fixed_types.h"
#include "sampling_algorithm.h"
#include "circular_queue.h"
#include "random.h"

#include <vector>

class PeriodicSampling : public SamplingAlgorithm
{
   protected:
      SubsecondTime m_detailed_interval;
      SubsecondTime m_fastforward_interval;
      SubsecondTime m_fastforward_sync_interval;
      SubsecondTime m_warmup_interval;
      SubsecondTime m_detailed_warmup_interval;

      bool m_detailed_sync;

      bool m_constant_ipc;
      std::vector<double> m_constant_ipcs;

      bool m_random_placement;
      SubsecondTime m_random_offset;
      Random m_prng;

      bool m_random_start;
      bool m_random_first;

      SubsecondTime m_periodic_last;
      SubsecondTime m_fastforward_time_remaining;
      SubsecondTime m_warmup_time_remaining;
      SubsecondTime m_detailed_warmup_time_remaining;

      UInt32 m_num_historic_cpi_intervals;
      std::vector<CircularQueue<SubsecondTime>* > m_historic_cpi_intervals;

      int m_dispatch_width;

      bool stepFastForward(SubsecondTime time, bool in_warmup);

   public:
      PeriodicSampling(SamplingManager *sampling_manager);

      virtual void callbackDetailed(SubsecondTime now);
      virtual void callbackFastForward(SubsecondTime now, bool in_warmup);
};

#endif /* __PERIODIC_SAMPLING */
