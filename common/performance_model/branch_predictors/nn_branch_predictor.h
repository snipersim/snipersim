#ifndef NNBRANCHPREDICTOR_H
#define NNBRANCHPREDICTOR_H

#include "branch_predictor.h"
#include <torch/torch.h>

struct BranchPredictorModel : torch::nn::Module {
  BranchPredictorModel() {
    fc1 = register_module("fc1", torch::nn::Linear(128, 8));
    fc2 = register_module("fc2", torch::nn::Linear(8, 4));
    fc3 = register_module("fc3", torch::nn::Linear(4, 1));
  }

  torch::Tensor forward(torch::Tensor x) {
    x = torch::relu(fc1->forward(x));
    x = torch::relu(fc2->forward(x));
    x = fc3->forward(x);
    return torch::sigmoid(x.reshape({-1}));
  }

  torch::nn::Linear fc1{nullptr}, fc2{nullptr}, fc3{nullptr};
};

class NNBranchPredictor : public BranchPredictor {
public:
    NNBranchPredictor(String name, core_id_t core_id, size_t batch_length, double learning_rate);

    bool predict(bool indirect, IntPtr ip, IntPtr target) override;
    void update(bool predicted, bool actual, bool indirect, IntPtr ip, IntPtr target) override;
private:
    const size_t batch_length;
    BranchPredictorModel model;
    torch::optim::Adam optimizer;
    std::vector<std::tuple<bool, bool, bool, IntPtr, IntPtr>> batch;
};
  
#endif // NNBRANCHPREDICTOR_H