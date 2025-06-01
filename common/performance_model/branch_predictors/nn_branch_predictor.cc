#include "nn_branch_predictor.h"

#include <torch/nn/cloneable.h>
#include <torch/nn/pimpl.h>
#include <torch/types.h>
#include <ATen/ATen.h>

torch::Tensor process_x(const std::vector<std::tuple<IntPtr, IntPtr>>& vector_x) {
    const long N = vector_x.size();
    torch::Tensor result = torch::zeros({N, LAST_N_BITS * 2}, torch::kFloat);
    auto accessor = result.accessor<float, 2>();
    for (long i = 0; i < N; i++) {
        IntPtr ip = std::get<0>(vector_x[i]);
        IntPtr target = std::get<1>(vector_x[i]);
        for (size_t bit = 0; bit < LAST_N_BITS; bit++) {
            if (ip & (1ll << bit)) {
                accessor[i][bit] = 1;
            } else {
                accessor[i][bit] = 0;
            }
        }
        for (size_t bit = 0; bit < LAST_N_BITS; bit++) {
            if (target & (1ll << bit)) {
                accessor[i][bit + LAST_N_BITS] = 1;
            } else {
                accessor[i][bit + LAST_N_BITS] = 0;
            }
        }
    }
    return result;
}

torch::Tensor process_y(const std::vector<bool>& vector_y) {
    const long N = vector_y.size();
    torch::Tensor result = torch::zeros({N}, torch::kFloat);
    auto accessor = result.accessor<float, 1>();
    for (long i = 0; i < N; i++) {
        if (vector_y[i]) {
            accessor[i] = 1;
        } else {
            accessor[i] = 0;
        }
    }
    return result;
}

NNBranchPredictor::NNBranchPredictor(String name, core_id_t core_id, size_t batch_length, double learning_rate) : 
    BranchPredictor(name, core_id), 
    batch_length(batch_length), 
    optimizer(model.parameters(), learning_rate), alternate_predictor(std::make_unique<PentiumMBranchPredictor>(name + "_alternate", core_id)) {}

bool NNBranchPredictor::predict(bool indirect, IntPtr ip, IntPtr target) {
    std::vector<std::tuple<IntPtr, IntPtr>> vector_x = {{ip, target}};
    torch::Tensor x = process_x(vector_x);
    auto [prediction, energy] = model.predict_with_energy(x);
    bool alternate_prediction = alternate_predictor->predict(indirect, ip, target);
    cummulated_energy += energy;
    // if (energy <= 0.2) {
        return prediction;
    // } else {
    // return alternate_prediction;
    // }
    // torch::Tensor y_pred= model.forward(x);
    // auto accessor = y_pred.accessor<float, 1>();
    // if (accessor[0] > 0.5) {
        // return true;
    // } else {
        // return false;
    // }
}

void NNBranchPredictor::update(bool predicted, bool actual, bool indirect, IntPtr ip, IntPtr target) {
    updateCounters(predicted, actual);
    if (predicted == actual) {
        correct++;
    } else {
        wrong++;
    }
    if ((correct + wrong) % 10000 == 0) {
        std::cerr << (double)correct/10000 << ' ' << cummulated_loss << ' ' << cummulated_energy << std::endl;
        correct = 0;
        wrong = 0;
        cummulated_loss = 0;
        cummulated_energy = 0;
    }

    batch.push_back({predicted, actual, indirect, ip, target});
    if (batch.size() == batch_length) {
        std::vector<std::tuple<IntPtr, IntPtr>> vector_x;
        std::vector<bool> vector_y;
        for (auto row : batch) {
            vector_x.push_back({std::get<3>(row), std::get<4>(row)});
            vector_y.push_back(std::get<1>(row));
        }
        torch::Tensor x = process_x(vector_x);
        torch::Tensor y = process_y(vector_y);

        optimizer.zero_grad();
        torch::Tensor y_pred = model.forward(x);

        torch::Tensor classification_loss = classification_loss_fn(y_pred, y);

        torch::Tensor energy_loss = model.compute_energy_loss(x, y);
        // torch::Tensor total_loss = energy_loss;
        torch::Tensor total_loss = classification_loss + energy_loss * 0.1;
        cummulated_loss += total_loss.item<double>();

        total_loss.backward();
        optimizer.step();
        batch.clear();
    }
    alternate_predictor->update(predicted, actual, indirect, ip, target);
}
