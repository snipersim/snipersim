#include "a53branchpredictor.h"
#include "simulator.h"
#include "config.hpp"

inline A53BranchPredictor::State nextState(A53BranchPredictor::State currentState, bool input) {
   switch (currentState) {
   case A53BranchPredictor::StronglyNotTaken:
      return input ? A53BranchPredictor::WeakelyTaken : A53BranchPredictor::StronglyNotTaken;
   case A53BranchPredictor::WeakelyNotTaken:
      return input ? A53BranchPredictor::WeakelyTaken : A53BranchPredictor::StronglyNotTaken;
   case A53BranchPredictor::WeakelyTaken:
      return input ? A53BranchPredictor::StronglyTaken : A53BranchPredictor::WeakelyNotTaken;
   case A53BranchPredictor::StronglyTaken:
      return input ? A53BranchPredictor::StronglyTaken : A53BranchPredictor::WeakelyTaken;
   }
   return A53BranchPredictor::StronglyNotTaken;
}

inline bool statePrediction(A53BranchPredictor::State state) {
   switch (state) {
   case A53BranchPredictor::StronglyNotTaken:
   case A53BranchPredictor::WeakelyNotTaken:
      return false;
   default:
      return true;
   }
}

A53BranchPredictor::A53BranchPredictor(String name, core_id_t core_id)
   : BranchPredictor(name, core_id)
   , m_num_registers(Sim()->getCfg()->getIntArray("perf_model/branch_predictor/num_history_registers", core_id))
   , size(Sim()->getCfg()->getIntArray("perf_model/branch_predictor/size", core_id))
   , m_pattern_history_table(std::vector<A53BranchPredictor::State>(m_num_registers*size, A53BranchPredictor::StronglyNotTaken))
   , m_branch_history_register(std::vector<int>(m_num_registers, 0))
{
}

void A53BranchPredictor::update(bool predicted, bool actual, bool indirect, IntPtr ip, IntPtr target) {
   updateCounters(predicted, actual);

   if (indirect) {
      ibtb.update(predicted, actual, indirect, ip, target);
      return;
   }

   char registerIndex = ip%m_num_registers;
   int registerValue = m_branch_history_register[registerIndex] & (size - 1);
   int historyIndex = registerValue + registerIndex*size;

   m_pattern_history_table[historyIndex] = nextState(m_pattern_history_table[historyIndex], actual);
   m_branch_history_register[registerIndex] = (registerValue << 1) | actual;
}

bool A53BranchPredictor::predict(bool indirect, IntPtr ip, IntPtr target) {

   if (indirect) {
      return ibtb.predict(indirect, ip, target);
   }

   char registerIndex = ip%m_num_registers;
   int registerValue = m_branch_history_register[registerIndex] & (size - 1);
   int historyIndex = registerValue + registerIndex*size;

   return statePrediction(m_pattern_history_table[historyIndex]);
}
