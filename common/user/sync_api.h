#ifndef SYNC_API_H
#define SYNC_API_H
#include <stdbool.h>

#include "subsecond_time.h"
#include "fixed_types.h"

#ifdef __cplusplus
extern "C" {
#endif

typedef SInt32 carbon_mutex_t;
typedef SInt32 carbon_cond_t;
typedef SInt32 carbon_barrier_t;

// These are the dummy functions that will get replaced
// by the simulator
// Related to Mutexes

void CarbonMutexInit(carbon_mutex_t *mux);
SubsecondTime CarbonMutexLock(carbon_mutex_t *mux, SubsecondTime delay = SubsecondTime::Zero());
SubsecondTime CarbonMutexTrylock(carbon_mutex_t *mux);
SubsecondTime CarbonMutexUnlock(carbon_mutex_t *mux, SubsecondTime delay = SubsecondTime::Zero());
bool CarbonIsMutexValid(carbon_mutex_t *mux);

// Related to condition variables
void CarbonCondInit(carbon_cond_t *cond);
SubsecondTime CarbonCondWait(carbon_cond_t *cond, carbon_mutex_t *mux);
SubsecondTime CarbonCondSignal(carbon_cond_t *cond);
SubsecondTime CarbonCondBroadcast(carbon_cond_t *cond);
bool CarbonIsCondValid(carbon_cond_t *cond);

// Related to barriers
void CarbonBarrierInit(carbon_barrier_t *barrier, unsigned int count);
SubsecondTime CarbonBarrierWait(carbon_barrier_t *barrier);
bool CarbonIsBarrierValid(carbon_barrier_t *barrier);

#ifdef __cplusplus
}
#endif

#endif
