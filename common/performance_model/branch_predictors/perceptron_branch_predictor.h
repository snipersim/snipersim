#ifndef PERCEPTRON_BRANCH_PREDICTOR_H
#define PERCEPTRON_BRANCH_PREDICTOR_H

/* The perceptron predictor (Jimenez and Lin, "Dynamic Branch Prediction with
   Perceptrons", HPCA 2001): a table of perceptrons indexed by the branch
   address, each a bias plus one signed weight per global-history bit, trained
   on a misprediction or when the output is within the threshold
   theta = 1.93 h + 14.  Ported from Virtuoso's port of ChampSim's. */

#include "branch_predictor.h"
#include "fixed_types.h"
#include <vector>
#include <cstdint>

class PerceptronBranchPredictor : public BranchPredictor
{
public:
   PerceptronBranchPredictor(String name, core_id_t core_id, UInt32 history_length, UInt32 num_perceptrons);

   bool predict(bool indirect, IntPtr ip, IntPtr target) override;
   void update(bool predicted, bool actual, bool indirect, IntPtr ip, IntPtr target) override;

private:
   UInt32 m_history_length;
   UInt32 m_num_perceptrons;
   int m_theta;
   std::vector<int8_t> m_weights;    // per perceptron: bias, then one weight per history bit
   std::vector<bool> m_history;      // newest first
   UInt64 m_trainings;               // perceptron.trainings: wrong, or |y| <= theta

   // The prediction being resolved: Sniper's core calls update() right
   // after predict() for the same branch.
   IntPtr m_last_ip;
   int m_last_output;
   bool m_have_last;

   int8_t *perceptron(IntPtr ip) { return &m_weights[(ip % m_num_perceptrons) * (m_history_length + 1)]; }
   int output(IntPtr ip);
};

#endif /* PERCEPTRON_BRANCH_PREDICTOR_H */
