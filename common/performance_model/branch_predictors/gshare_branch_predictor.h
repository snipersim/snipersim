#ifndef GSHARE_BRANCH_PREDICTOR_H
#define GSHARE_BRANCH_PREDICTOR_H

/* gshare (McFarling, "Combining Branch Predictors", 1993): a table of 2-bit
   saturating counters indexed by the branch address XORed with the global
   history.  Ported from Virtuoso's port of ChampSim's gshare. */

#include "branch_predictor.h"
#include "fixed_types.h"
#include <vector>
#include <cstdint>

class GshareBranchPredictor : public BranchPredictor
{
public:
   GshareBranchPredictor(String name, core_id_t core_id, UInt32 history_length, UInt32 table_size);

   bool predict(bool indirect, IntPtr ip, IntPtr target) override;
   void update(bool predicted, bool actual, bool indirect, IntPtr ip, IntPtr target) override;

private:
   UInt32 m_history_length;
   UInt32 m_table_mask;
   uint64_t m_history_mask;
   uint64_t m_history;
   std::vector<uint8_t> m_counters;   // 0..3, taken at 2 and above
   UInt64 m_updates;                  // gshare.updates: counters trained

   UInt32 index(IntPtr ip) const;
};

#endif /* GSHARE_BRANCH_PREDICTOR_H */
