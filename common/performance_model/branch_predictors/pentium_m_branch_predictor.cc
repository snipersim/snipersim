
#include "simulator.h"
#include "pentium_m_branch_predictor.h"

PentiumMBranchPredictor::PentiumMBranchPredictor(String name, core_id_t core_id)
   : BranchPredictor(name, core_id)
   , m_pir(0)
{
}

PentiumMBranchPredictor::~PentiumMBranchPredictor()
{
}

bool PentiumMBranchPredictor::predict(IntPtr ip, IntPtr target)
{
   IntPtr hash = hash_function(ip, m_pir);

   BranchPredictorReturnValue global_pred_out = m_global_predictor.lookup(hash, target);
   BranchPredictorReturnValue btb_out = m_btb.lookup(ip, target);
   BranchPredictorReturnValue lpb_out = m_lpb.lookup(ip, target);
   bool bimodal_out = m_bimodal_table.predict(ip, target);

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
   IntPtr hash = hash_function(ip, m_pir);

   updateCounters(predicted, actual);
   m_global_predictor.update(predicted, actual, hash, target);
   m_btb.update(predicted, actual, ip, target);
   m_lpb.update(predicted, actual, ip, target);
   m_bimodal_table.update(predicted, actual, ip, target);
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
      rhs = 0;
   }

   m_pir = (m_pir << 2) ^ rhs;
}

IntPtr PentiumMBranchPredictor::hash_function(IntPtr ip, IntPtr pir)
{

   IntPtr top = ((ip >> 13) ^ (pir)) & 0x3F;
   IntPtr bottom = ((ip >> 4) ^ (pir >> 6)) & 0x1FF;

   return ((top << 9) | bottom);
}

