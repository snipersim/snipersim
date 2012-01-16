#ifndef ROUTINE_REPLACE_H
#define ROUTINE_REPLACE_H

#include "fixed_types.h"
#include "pin.H"

// Helper macros
// In Pin 2.8, IARG_END became a macro that contains additional IARG_* enums.
// Detect if IARG_END is a macro to enable correct parsing of parameter lists.
#if defined IARG_END
# define PIN_USES_IARG_LAST 1
#else
# define PIN_USES_IARG_LAST 0
#endif

bool replaceUserAPIFunction(RTN& rtn, string& name, INS& ins);

void setupCarbonSpawnThreadSpawnerStack (CONTEXT *ctx);
void setupCarbonThreadSpawnerStack (CONTEXT *ctx);

// Thread spawning and management
void replacementMain (CONTEXT *ctxt);
void afterMain (CONTEXT *ctxt);
void replacementGetThreadToSpawn (CONTEXT *ctxt);
void replacementThreadStartNull (CONTEXT *ctxt);
void replacementThreadExitNull (CONTEXT *ctxt);
void replacementGetCoreId (CONTEXT *ctxt);
void replacementDequeueThreadSpawnRequest (CONTEXT *ctxt);

// Pin specific stack management
void replacementPthreadAttrInitOtherAttr (CONTEXT *ctxt);

// Carbon API
void replacementStartSimNull (CONTEXT *ctxt);
void replacementStopSim (CONTEXT *ctxt);
void replacementSpawnThread (CONTEXT *ctxt);
void replacementJoinThread (CONTEXT *ctxt);

void replacementMutexInit(CONTEXT *ctxt);
void replacementMutexLock(CONTEXT *ctxt);
void replacementMutexUnlock(CONTEXT *ctxt);

void replacementCondInit(CONTEXT *ctxt);
void replacementCondWait(CONTEXT *ctxt);
void replacementCondSignal(CONTEXT *ctxt);
void replacementCondBroadcast(CONTEXT *ctxt);

void replacementBarrierInit(CONTEXT *ctxt);
void replacementBarrierWait(CONTEXT *ctxt);

// CAPI
void replacement_CAPI_Initialize (CONTEXT *ctxt);
void replacement_CAPI_rank (CONTEXT *ctxt);
void replacement_CAPI_message_send_w (CONTEXT *ctxt);
void replacement_CAPI_message_receive_w (CONTEXT *ctxt);
void replacement_CAPI_message_send_w_ex (CONTEXT *ctxt);
void replacement_CAPI_message_receive_w_ex (CONTEXT *ctxt);

// pthread
void replacementPthreadCreate (CONTEXT *ctxt);
void replacementPthreadSelf (CONTEXT *ctxt);
void replacementPthreadJoin (CONTEXT *ctxt);
void replacementPthreadExitNull (CONTEXT *ctxt);
void replacementPthreadMutexInit (CONTEXT *ctxt);
void replacementPthreadMutexLock (CONTEXT *ctxt);
void replacementPthreadMutexTrylock (CONTEXT *ctxt);
void replacementPthreadMutexUnlock (CONTEXT *ctxt);
void replacementPthreadMutexDestroy(CONTEXT *ctxt);
void replacementPthreadCondInit (CONTEXT *ctxt);
void replacementPthreadCondWait (CONTEXT *ctxt);
void replacementPthreadCondSignal (CONTEXT *ctxt);
void replacementPthreadCondBroadcast (CONTEXT *ctxt);
void replacementPthreadCondDestroy(CONTEXT *ctxt);
void replacementPthreadBarrierInit (CONTEXT *ctxt);
void replacementPthreadBarrierWait (CONTEXT *ctxt);
void replacementPthreadBarrierDestroy(CONTEXT *ctxt);

// Cache Counters
void replacementResetCacheCounters(CONTEXT *ctxt);
void replacementDisableCacheCounters(CONTEXT *ctxt);

// Misc. system services
void replacementGetNprocs(CONTEXT *ctxt);

void initialize_replacement_args (CONTEXT *ctxt, ...);
void retFromReplacedRtn (CONTEXT *ctxt, ADDRINT ret_val);
#endif
