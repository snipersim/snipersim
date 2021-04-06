#ifndef A53BRANCHPREDICTOR_H
#define A53BRANCHPREDICTOR_H

#include "branch_predictor.h"
#include "pentium_m_indirect_branch_target_buffer.h"
#include <vector>

class A53BranchPredictor : public BranchPredictor {

public:
    enum State {
        StronglyNotTaken,
        WeakelyTaken,
        WeakelyNotTaken,
        StronglyTaken
    };

    A53BranchPredictor(String name, core_id_t core_id);

    bool predict(bool indirect, IntPtr ip, IntPtr target);
    void update(bool predicted, bool actual, bool indirect, IntPtr ip, IntPtr target);
private:
    const int m_num_registers;
    const int size;

    PentiumMIndirectBranchTargetBuffer ibtb;
    std::vector<State> m_pattern_history_table;
    std::vector<int> m_branch_history_register;
};

#endif // A53BRANCHPREDICTOR_H
