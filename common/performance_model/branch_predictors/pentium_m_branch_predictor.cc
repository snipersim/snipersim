
#include "simulator.h"
#include "pentium_m_branch_predictor.h"

PentiumMBranchPredictor::PentiumMBranchPredictor(String name, core_id_t core_id)
   : BranchPredictor(name, core_id)
   , m_pir(0)
   , m_last_gp_hit(false)
   , m_last_lpb_hit(false)
{
}

PentiumMBranchPredictor::~PentiumMBranchPredictor()
{
}

bool PentiumMBranchPredictor::predict(IntPtr ip, IntPtr target)
{
   BranchPredictorReturnValue global_pred_out = m_global_predictor.lookup(ip, target, m_pir);
   BranchPredictorReturnValue btb_out = m_btb.lookup(ip, target);
   BranchPredictorReturnValue lpb_out = m_lpb.lookup(ip, target);
   bool bimodal_out = m_bimodal_table.predict(ip, target);

   m_last_gp_hit = global_pred_out.hit;
   m_last_lpb_hit = lpb_out.hit & btb_out.hit;

   // Outcome prediction logic
   bool result;
   if (global_pred_out.hit)
   {
      result = global_pred_out.prediction;
   }
   else if (lpb_out.hit & btb_out.hit)
   {
      result = lpb_out.prediction;
   }
   else
   {
      result = bimodal_out;
   }

   // TODO FIXME: Failed matches against the target address should force a branch or fetch miss

   return result;
}

void PentiumMBranchPredictor::update(bool predicted, bool actual, IntPtr ip, IntPtr target)
{
   updateCounters(predicted, actual);
   m_btb.update(predicted, actual, ip, target);
   m_lpb.update(predicted, actual, ip, target);
   if (!m_last_gp_hit && !m_last_lpb_hit) // Update bimodal predictor only when global and loop predictors missed
      m_bimodal_table.update(predicted, actual, ip, target);
   if (predicted != actual) // Update global predictor only when other predictors were wrong
      m_global_predictor.update(predicted, actual, ip, target, m_pir);
   // TODO FIXME: Properly propagate the branch type information from the decoder (IndirectBranch information)
   update_pir(actual, ip, target, BranchPredictorReturnValue::ConditionalBranch);
}

void PentiumMBranchPredictor::update_pir(bool actual, IntPtr ip, IntPtr target, BranchPredictorReturnValue::BranchType branch_type)
{
   IntPtr rhs;

   if ((branch_type == BranchPredictorReturnValue::ConditionalBranch) & actual)
   {
      rhs = ip >> 4;
   }
   else if (branch_type == BranchPredictorReturnValue::IndirectBranch)
   {
      rhs = (ip >> 4) | target;
   }
   else
   {
      // No PIR update
      return;
   }

   m_pir = ((m_pir << 2) ^ rhs) & 0x7fff;
}
