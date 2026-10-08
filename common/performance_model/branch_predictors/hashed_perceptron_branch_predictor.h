#ifndef HASHED_PERCEPTRON_BRANCH_PREDICTOR_H
#define HASHED_PERCEPTRON_BRANCH_PREDICTOR_H

/* The hashed perceptron (Tarjan and Skadron, "Merging Path and Gshare
   Indexing in Perceptron Branch Prediction", TACO 2005, in ChampSim's
   geometric-history form): sixteen weight tables, each indexed by the branch
   address XORed with the global history folded to 12 bits over a geometric
   series of lengths (0 to 232), summed; with Seznec's dynamic threshold.
   Ported from Virtuoso's port of ChampSim's. */

#include "branch_predictor.h"
#include "fixed_types.h"
#include "folded_history.h"
#include <array>
#include <vector>
#include <cstdint>

class HashedPerceptronBranchPredictor : public BranchPredictor
{
public:
   HashedPerceptronBranchPredictor(String name, core_id_t core_id);

   bool predict(bool indirect, IntPtr ip, IntPtr target) override;
   void update(bool predicted, bool actual, bool indirect, IntPtr ip, IntPtr target) override;

private:
   static constexpr size_t NTABLES = 16;
   static constexpr size_t INDEX_BITS = 12;
   static constexpr size_t TABLE_SIZE = 1 << INDEX_BITS;
   static constexpr int SPEED = 18;
   static constexpr std::array<size_t, NTABLES> HISTORY_LENGTHS = {
      0, 3, 4, 6, 8, 10, 14, 19, 26, 36, 49, 67, 91, 125, 170, 232 };

   std::array<std::array<int8_t, TABLE_SIZE>, NTABLES> m_tables;
   std::vector<FoldedHistory> m_history;
   int m_theta;
   int m_tc;
   UInt64 m_trainings;               // hashed_perceptron.trainings

   std::array<UInt32, NTABLES> m_last_index;
   int m_last_output;
};

#endif /* HASHED_PERCEPTRON_BRANCH_PREDICTOR_H */
