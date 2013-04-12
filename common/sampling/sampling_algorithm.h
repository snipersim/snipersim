#ifndef __SAMPLING_ALGORITHM
#define __SAMPLING_ALGORITHM

#include "fixed_types.h"
#include "subsecond_time.h"

class SamplingManager;

// Base class for algorithms that decide how to sample
// - an algorithm receives a periodic callback, both while in detailed and in fastforward mode
// - during each callback the algorithm can examine system state, and decide to switch modes
//   by calling SamplingManager::{enable|disable}FastForward
// - the algorithm should perform the proper setup (configure each core's setCurrentCPI)
//   so time can be maintained while fast-forwarding

class SamplingAlgorithm
{
protected:
   SamplingManager *m_sampling_manager;
public:
   SamplingAlgorithm(SamplingManager *sampling_manager) : m_sampling_manager(sampling_manager) {}
   virtual ~SamplingAlgorithm() {}

   virtual void callbackDetailed(SubsecondTime now) = 0;
   virtual void callbackFastForward(SubsecondTime now, bool in_warmup) = 0;

   static SamplingAlgorithm* create(SamplingManager *sampling_manager);
};

#endif /* __SAMPLING_ALGORITHM */
