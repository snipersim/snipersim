#include "gshare_branch_predictor.h"
#include "log.h"
#include "stats.h"

GshareBranchPredictor::GshareBranchPredictor(String name, core_id_t core_id, UInt32 history_length, UInt32 table_size)
   : BranchPredictor(name, core_id)
   , m_history_length(history_length)
   , m_table_mask(table_size - 1)
   , m_history_mask((1ULL << history_length) - 1)
   , m_history(0)
   , m_counters(table_size, 1)        // weakly not taken
   , m_updates(0)
{
   registerStatsMetric("gshare", core_id, "updates", &m_updates);
   LOG_ASSERT_ERROR(table_size > 0 && (table_size & (table_size - 1)) == 0,
                    "gshare: table_size must be a power of two (got %u)", table_size);
   LOG_ASSERT_ERROR(history_length >= 1 && history_length <= 63,
                    "gshare: history_length must be 1..63 (got %u)", history_length);
}

UInt32 GshareBranchPredictor::index(IntPtr ip) const
{
   /* The history XORed with three slices of the address, as ChampSim does,
      so that high address bits also reach the index. */
   uint64_t hash = m_history;
   hash ^= ip & m_table_mask;
   hash ^= (ip >> m_history_length) & m_history_mask;
   hash ^= (ip >> (2 * m_history_length)) & m_history_mask;
   return hash & m_table_mask;
}

bool GshareBranchPredictor::predict(bool indirect, IntPtr ip, IntPtr target)
{
   return m_counters[index(ip)] >= 2;
}

void GshareBranchPredictor::update(bool predicted, bool actual, bool indirect, IntPtr ip, IntPtr target)
{
   updateCounters(predicted, actual);

   uint8_t &ctr = m_counters[index(ip)];
   ++m_updates;
   if (actual && ctr < 3)
      ++ctr;
   else if (!actual && ctr > 0)
      --ctr;

   m_history = ((m_history << 1) | (actual ? 1 : 0)) & m_history_mask;
}
