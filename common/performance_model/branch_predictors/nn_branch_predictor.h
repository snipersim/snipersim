#ifndef NNBRANCHPREDICTOR_H
#define NNBRANCHPREDICTOR_H

#include "branch_predictor.h"
#include "pentium_m_branch_predictor.h"
#include <torch/torch.h>
#include <list>
#include <utility>
#include <unordered_map>
#include <memory>

const size_t LAST_N_BITS = 4;

struct BranchPredictorModel : torch::nn::Module {
  BranchPredictorModel() {

    feature_extractor = torch::nn::Sequential(
      torch::nn::Linear(LAST_N_BITS * 2, 8),
      torch::nn::ReLU(),
      torch::nn::Linear(8, 4),
      torch::nn::ReLU()
    );
    feature_extractor = register_module("feature_extractor", feature_extractor);
    classifier = register_module("classifier", torch::nn::Linear(4, 1));
    energy_true = register_module("energy_true", torch::nn::Linear(4, 1));
    energy_false = register_module("energy_false", torch::nn::Linear(4, 1));
    torch::nn::init::xavier_uniform_(energy_true->weight);
    torch::nn::init::xavier_uniform_(energy_false->weight);
    torch::nn::init::constant_(energy_true->bias, 0.1);
    torch::nn::init::constant_(energy_false->bias, 0.1);
  }

  torch::Tensor forward(torch::Tensor x) {
    torch::Tensor logits = classifier->forward(feature_extractor->forward(x));
    return torch::sigmoid(logits.reshape({-1}));
  }

  torch::Tensor compute_energy_loss(torch::Tensor x, torch::Tensor y) {
    torch::Tensor features = feature_extractor->forward(x);
    torch::Tensor energy_pos = energy_true->forward(features);
    torch::Tensor energy_neg = energy_false->forward(features);

    // Energy loss: hinge loss formulation
    torch::Tensor margin = torch::ones_like(energy_pos); // Margin hyperparameter
    torch::Tensor loss_pos = torch::relu(energy_pos - energy_neg + margin);
    torch::Tensor loss_neg = torch::relu(energy_neg - energy_pos + margin);

    // Combine losses for true and false labels
    torch::Tensor energy_loss = torch::where(y == 1, loss_pos, loss_neg);

    return energy_loss.mean();
  }

  std::pair<bool, double> predict_with_energy(torch::Tensor x) {
      torch::Tensor features = feature_extractor->forward(x);
      torch::Tensor logits = classifier->forward(feature_extractor->forward(x));
      bool prediction = torch::sigmoid(logits.reshape({-1})).item<float>() > 0.5;

      double energy = 0;
      torch::Tensor energy_pos = energy_true->forward(features).squeeze();
      torch::Tensor energy_neg = energy_false->forward(features).squeeze();
      if (prediction) {
        energy = torch::relu(energy_pos - energy_neg + 1).item<double>();
      } else {
        energy = torch::relu(energy_neg - energy_pos + 1).item<double>();
      }

      return {prediction, energy};
  }

  torch::nn::Sequential feature_extractor{nullptr};
  torch::nn::Linear classifier{nullptr};
  torch::nn::Linear energy_true{nullptr};
  torch::nn::Linear energy_false{nullptr};
};

class NNBranchPredictor : public BranchPredictor {
public:
    NNBranchPredictor(String name, core_id_t core_id, size_t batch_length, double learning_rate);

    bool predict(bool indirect, IntPtr ip, IntPtr target) override;
    void update(bool predicted, bool actual, bool indirect, IntPtr ip, IntPtr target) override;
private:
    const size_t embedding_length = 1024;
    const size_t history_length = 64;
    const size_t batch_length;
    // EmbeddingTable embeddingTable;
    // std::deque<IntPtr> history;
    BranchPredictorModel model;
    torch::optim::Adam optimizer;
    torch::nn::BCELoss classification_loss_fn;
    torch::nn::MSELoss energy_loss_fn;
    std::vector<std::tuple<bool, bool, bool, IntPtr, IntPtr>> batch;
    double cummulated_loss = 0;
    double cummulated_energy = 0;
    int correct = 0;
    int wrong = 0;
    std::unique_ptr<BranchPredictor> alternate_predictor;
};
  
#endif // NNBRANCHPREDICTOR_H
