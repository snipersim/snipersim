#include "fixed_types.h"
#include "codecache_trace.h"
#include "timer.h"
#include "hooks_manager.h"
#include "subsecond_time.h"
#include "simulator.h"

#include "pin.H"

#define __STDC_FORMAT_MACROS // PRIu64
#include <inttypes.h>
#include <stdio.h>
#include <iostream>

uint64_t cc_counters[8];
static FILE* cctrace;
static FILE* ccstats;
SubsecondTime next_callback;

#define atomic_inc_int64(a) __sync_fetch_and_add(&(a), 1)

VOID cacheInit()
{
}

VOID newCacheBlock(USIZE new_block_size)
{
   atomic_inc_int64(cc_counters[0]);
}

VOID fullCache(USIZE trace_size, USIZE stub_size)
{
   atomic_inc_int64(cc_counters[1]);
}

VOID cacheFlushed()
{
   atomic_inc_int64(cc_counters[2]);
}

VOID codeCacheEntered(ADDRINT cache_pc)
{
   atomic_inc_int64(cc_counters[3]);
}

VOID codeCacheExited(ADDRINT cache_pc)
{
}

VOID traceLinked(ADDRINT branch_pc, ADDRINT target_pc)
{
   atomic_inc_int64(cc_counters[4]);
}

VOID traceUnlinked(ADDRINT branch_pc, ADDRINT stub_pc)
{
   atomic_inc_int64(cc_counters[5]);
}

VOID traceInvalidated(ADDRINT orig_pc, ADDRINT cache_pc, BOOL success)
{
   atomic_inc_int64(cc_counters[6]);
}

VOID traceInserted(TRACE trace, VOID *v)
{
   atomic_inc_int64(cc_counters[7]);
}

void printCodeCacheTrace(const SubsecondTime& time)
{
   fprintf(cctrace, "%" PRIu64, time.getNS());
   for (uint32_t i = 0 ; i < 8 ; i++)
   {
      fprintf(cctrace, " %" PRIu64, cc_counters[i]);
   }
   fprintf(cctrace, "\n");
}

void printCodeCacheStats(const SubsecondTime& time)
{
   fprintf(ccstats, "%" PRIu64, time.getNS());
   fprintf(ccstats, " %u", CODECACHE_CodeMemReserved());
   fprintf(ccstats, " %u", CODECACHE_DirectoryMemUsed());
   fprintf(ccstats, " %u", CODECACHE_CodeMemUsed());
   fprintf(ccstats, " %u", CODECACHE_ExitStubBytes());
   fprintf(ccstats, " %u", CODECACHE_LinkBytes());
   fprintf(ccstats, " %u", CODECACHE_CacheSizeLimit());
   fprintf(ccstats, " %u", CODECACHE_BlockSize());
   fprintf(ccstats, " %u", CODECACHE_NumTracesInCache());
   fprintf(ccstats, " %u", CODECACHE_NumExitStubsInCache());
   fprintf(ccstats, "\n");
}

void codeCachePeriodicCallback(void *self, subsecond_time_t _time)
{
   SubsecondTime time(_time);
   if (time < next_callback)
      return;

   next_callback = time + SubsecondTime::NS(10000);

   printCodeCacheTrace(time);
   printCodeCacheStats(time);
}

void initCodeCacheTracing()
{
   cctrace = fopen(Sim()->getConfig()->formatOutputFileName("cctrace.txt").c_str(), "w");
   LOG_ASSERT_ERROR(cctrace != NULL, "Expected to be able to create a file");

   ccstats = fopen(Sim()->getConfig()->formatOutputFileName("ccstats.txt").c_str(), "w");
   LOG_ASSERT_ERROR(cctrace != NULL, "Expected to be able to create a file");

   next_callback = SubsecondTime::Zero();

   Sim()->getHooksManager()->registerHook(HookType::HOOK_PERIODIC, (HooksManager::HookCallbackFunc)codeCachePeriodicCallback, (UInt64)NULL);

   //CODECACHE_AddCacheInitFunction(cacheInit, NULL);
   CODECACHE_AddCacheBlockFunction(newCacheBlock, NULL);
   CODECACHE_AddFullCacheFunction(fullCache, NULL);
   CODECACHE_AddCacheFlushedFunction(cacheFlushed, NULL);
   CODECACHE_AddCodeCacheEnteredFunction(codeCacheEntered, NULL);
   //CODECACHE_AddCodeCacheExitedFunction(codeCacheExited, NULL);
   CODECACHE_AddTraceLinkedFunction(traceLinked, NULL);
   CODECACHE_AddTraceUnlinkedFunction(traceUnlinked, NULL);
   CODECACHE_AddTraceInvalidatedFunction(traceInvalidated, NULL);
   CODECACHE_AddTraceInsertedFunction(traceInserted, NULL);
}
