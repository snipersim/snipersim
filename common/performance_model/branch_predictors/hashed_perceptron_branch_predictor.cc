#include "hashed_perceptron_branch_predictor.h"
#include "stats.h"
#include <cstdlib>

constexpr std::array<size_t, HashedPerceptronBranchPredictor::NTABLES> HashedPerceptronBranchPredictor::HISTORY_LENGTHS;

HashedPerceptronBranchPredictor::HashedPerceptronBranchPredictor(String name, core_id_t core_id)
   : BranchPredictor(name, core_id)
   , m_theta(10)
   , m_tc(0)
   , m_trainings(0)
   , m_last_output(0)
{
   registerStatsMetric("hashed_perceptron", core_id, "trainings", &m_trainings);
   for (auto &t : m_tables)
      t.fill(0);
   for (size_t i = 0; i < NTABLES; ++i)
      m_history.emplace_back(HISTORY_LENGTHS[i], INDEX_BITS);
   m_last_index.fill(0);
}

bool HashedPerceptronBranchPredictor::predict(bool indirect, IntPtr ip, IntPtr target)
{
   int y = 0;
   for (size_t i = 0; i < NTABLES; ++i)
   {
      m_last_index[i] = (m_history[i].value() ^ ip) & (TABLE_SIZE - 1);
      y += m_tables[i][m_last_index[i]];
   }
   m_last_output = y;
   return y >= 1;
}

void HashedPerceptronBranchPredictor::update(bool predicted, bool actual, bool indirect, IntPtr ip, IntPtr target)
{
   updateCounters(predicted, actual);

   bool correct = (m_last_output >= 1) == actual;
   if (!correct || std::abs(m_last_output) < m_theta)
   {
      ++m_trainings;
      for (size_t i = 0; i < NTABLES; ++i)
      {
         int8_t &w = m_tables[i][m_last_index[i]];
         if (actual && w < 127)
            ++w;
         else if (!actual && w > -128)
            --w;
      }
      // Seznec's dynamic threshold: more training after mispredictions,
      // less after correct but weak outputs.
      if (!correct && ++m_tc >= SPEED)
      {
         ++m_theta;
         m_tc = 0;
      }
      else if (correct && --m_tc <= -SPEED)
      {
         --m_theta;
         m_tc = 0;
      }
   }

   for (auto &h : m_history)
      h.push(actual);
}
