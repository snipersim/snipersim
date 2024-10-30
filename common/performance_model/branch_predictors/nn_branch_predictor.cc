#include "nn_branch_predictor.h"

#include <torch/nn/cloneable.h>
#include <torch/nn/pimpl.h>
#include <torch/types.h>
#include <ATen/ATen.h>

torch::Tensor process_x(const std::vector<std::tuple<IntPtr, IntPtr>>& vector_x) {
    const long N = vector_x.size();
    torch::Tensor result = torch::zeros({N, 128}, torch::kFloat);
    auto accessor = result.accessor<float, 2>();
    for (long i = 0; i < N; i++) {
        IntPtr ip = std::get<0>(vector_x[i]);
        IntPtr target = std::get<1>(vector_x[i]);
        for (size_t bit = 0; bit < 64; bit++) {
            if (ip & (1ll << bit)) {
                accessor[i][bit] = 1;
            } else {
                accessor[i][bit] = 0;
            }
        }
        for (size_t bit = 0; bit < 64; bit++) {
            if (target & (1ll << bit)) {
                accessor[i][bit + 64] = 1;
            } else {
                accessor[i][bit + 64] = 0;
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
    optimizer{model.parameters(), learning_rate} {}

bool NNBranchPredictor::predict(bool indirect, IntPtr ip, IntPtr target) {
    std::vector<std::tuple<IntPtr, IntPtr>> vector_x = {{ip, target}};
    torch::Tensor x = process_x(vector_x);
    torch::Tensor y_pred= model.forward(x);
    auto accessor = y_pred.accessor<float, 1>();
    if (accessor[0] > 0.5) {
        return true;
    } else {
        return false;
    }
}

void NNBranchPredictor::update(bool predicted, bool actual, bool indirect, IntPtr ip, IntPtr target) {
    updateCounters(predicted, actual);
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

        torch::Tensor loss = torch::binary_cross_entropy(y_pred, y);
        loss.backward();
        optimizer.step();
        batch.clear();
    }
}
