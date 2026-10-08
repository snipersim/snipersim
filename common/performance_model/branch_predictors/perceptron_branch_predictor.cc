#include "perceptron_branch_predictor.h"
#include "log.h"
#include "stats.h"
#include <cmath>
#include <cstdlib>

static inline void bump(int8_t &w, bool up)
{
   if (up && w < 127)
      ++w;
   else if (!up && w > -128)
      --w;
}

PerceptronBranchPredictor::PerceptronBranchPredictor(String name, core_id_t core_id, UInt32 history_length, UInt32 num_perceptrons)
   : BranchPredictor(name, core_id)
   , m_history_length(history_length)
   , m_num_perceptrons(num_perceptrons)
   , m_theta((int)std::lround(1.93 * history_length + 14))
   , m_weights((size_t)num_perceptrons * (history_length + 1), 0)
   , m_history(history_length, false)
   , m_trainings(0)
   , m_last_ip(0), m_last_output(0), m_have_last(false)
{
   registerStatsMetric("perceptron", core_id, "trainings", &m_trainings);
   LOG_ASSERT_ERROR(num_perceptrons > 0 && history_length > 0,
                    "perceptron: history_length and num_perceptrons must be positive");
}

int PerceptronBranchPredictor::output(IntPtr ip)
{
   const int8_t *p = perceptron(ip);
   int y = p[0];
   for (UInt32 i = 0; i < m_history_length; ++i)
      y += m_history[i] ? p[i + 1] : -p[i + 1];
   return y;
}

bool PerceptronBranchPredictor::predict(bool indirect, IntPtr ip, IntPtr target)
{
   m_last_ip = ip;
   m_last_output = output(ip);
   m_have_last = true;
   return m_last_output >= 0;
}

void PerceptronBranchPredictor::update(bool predicted, bool actual, bool indirect, IntPtr ip, IntPtr target)
{
   updateCounters(predicted, actual);

   int y = (m_have_last && m_last_ip == ip) ? m_last_output : output(ip);
   m_have_last = false;

   /* Train on the history the prediction was made with, then shift the
      outcome in.  Upstream kept a speculative history and a 100-entry state
      buffer for delayed updates, which Sniper never makes, and when a state
      was not found it trained without advancing the history at all. */
   if ((y >= 0) != actual || std::abs(y) <= m_theta)
   {
      ++m_trainings;
      int8_t *p = perceptron(ip);
      bump(p[0], actual);
      for (UInt32 i = 0; i < m_history_length; ++i)
         bump(p[i + 1], m_history[i] == actual);
   }
   for (UInt32 i = m_history_length - 1; i > 0; --i)
      m_history[i] = m_history[i - 1];
   m_history[0] = actual;
}
