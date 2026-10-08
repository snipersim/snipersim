#ifndef TAGE_BRANCH_PREDICTOR_H
#define TAGE_BRANCH_PREDICTOR_H

/* TAGE (Seznec and Michaud, "A case for (partially) TAgged GEometric history
   length branch prediction", JILP 2006): a bimodal base predictor and twelve
   partially tagged tables indexed with global history over geometric lengths
   (4 to 640 branches), the longest hitting table providing the prediction,
   with an alternate prediction for newly allocated entries and allocation of
   a longer-history entry on a misprediction.  Ported from Virtuoso's port. */

#include "branch_predictor.h"
#include "fixed_types.h"
#include "folded_history.h"
#include <array>
#include <vector>
#include <cstdint>

class TAGEBranchPredictor : public BranchPredictor
{
public:
   TAGEBranchPredictor(String name, core_id_t core_id);

   bool predict(bool indirect, IntPtr ip, IntPtr target) override;
   void update(bool predicted, bool actual, bool indirect, IntPtr ip, IntPtr target) override;

private:
   static constexpr int NHIST = 12;
   static constexpr int LOGB = 13;        // bimodal: 8K 2-bit counters
   static constexpr int LOGG = 10;        // each tagged table: 1K entries
   static constexpr int TBITS = 9;        // partial tag width
   static constexpr int CTR_MAX = 3, CTR_MIN = -4;   // 3-bit signed prediction counter
   static constexpr int U_MAX = 3;                   // 2-bit unsigned useful counter
   static constexpr int PHIST_BITS = 16;
   static constexpr std::array<int, NHIST> HIST_LENGTHS = {
      4, 6, 10, 16, 25, 40, 64, 101, 160, 254, 403, 640 };

   struct Entry
   {
      int8_t ctr = 0;
      uint16_t tag = 0;
      uint8_t useful = 0;
      bool valid = false;
   };

   std::vector<uint8_t> m_bimodal;                    // 0..3
   std::array<std::vector<Entry>, NHIST> m_tables;
   std::vector<FoldedHistory> m_fold_index, m_fold_tag1, m_fold_tag2;
   uint64_t m_phist;
   int m_use_alt;                                     // 0..15, use the alternate when >= 8
   uint32_t m_seed;
   UInt64 m_tagged_provided;         // tage.tagged_provided: predictions from a tagged table
   UInt64 m_allocations;             // tage.allocations: entries allocated on mispredictions

   // The prediction being resolved (update follows predict for one branch).
   struct State
   {
      IntPtr ip = 0;
      bool valid = false;
      int provider = -1, alt = -1;
      bool provider_pred = false, alt_pred = false, pred = false;
      std::array<UInt32, NHIST> index{};
      std::array<uint16_t, NHIST> tag{};
   } m_state;

   UInt32 index(IntPtr ip, int t) const;
   uint16_t tag(IntPtr ip, int t) const;
   void lookup(IntPtr ip);
   uint32_t rnd();
};

#endif /* TAGE_BRANCH_PREDICTOR_H */
