#ifndef THREAD_SUPPORT_PRIVATE_H
#define THREAD_SUPPORT_PRIVATE_H

#include "thread_support.h"

#ifdef __cplusplus
extern "C" {
#endif

void CarbonGetThreadToSpawn(ThreadSpawnRequest *req);
void CarbonDequeueThreadSpawnReq (ThreadSpawnRequest *req);
void CarbonPthreadAttrInitOtherAttr(pthread_attr_t *attr);

#ifdef __cplusplus
}
#endif

#endif
