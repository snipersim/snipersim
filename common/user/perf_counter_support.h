#ifndef __PERF_COUNTER_SUPPORT_H__
#define __PERF_COUNTER_SUPPORT_H__

#ifdef __cplusplus
extern "C" {
#endif

void CarbonResetCacheCounters(void);
void CarbonDisableCacheCounters(void);

#ifdef __cplusplus
}
#endif

#endif /* __PERF_COUNTER_SUPPORT_H__ */
