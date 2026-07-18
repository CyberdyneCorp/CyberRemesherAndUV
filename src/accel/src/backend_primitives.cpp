#include <mutex>

#include "cyber/accel/backend.hpp"

// CPU reference implementations of IBackend's accelerated numeric primitives.
// These live on the base class so every backend inherits a correct version and
// GPU backends only override the ones they accelerate.
namespace cyber::accel {

void IBackend::axpy(float alpha, const float* x, float* y, std::size_t n) {
    parallelFor(0, n, [alpha, x, y](std::size_t lo, std::size_t hi) {
        for (std::size_t i = lo; i < hi; ++i) {
            y[i] += alpha * x[i];
        }
    });
}

float IBackend::dot(const float* x, const float* y, std::size_t n) {
    double total = 0.0;  // accumulate in double to keep the reference accurate
    std::mutex mutex;
    parallelFor(0, n, [&](std::size_t lo, std::size_t hi) {
        double local = 0.0;
        for (std::size_t i = lo; i < hi; ++i) {
            local += static_cast<double>(x[i]) * static_cast<double>(y[i]);
        }
        const std::lock_guard<std::mutex> lock(mutex);
        total += local;
    });
    return static_cast<float>(total);
}

void IBackend::spmvCsr(std::size_t rows, const std::size_t* rowStart, const std::size_t* colIndex,
                       const float* value, const float* x, float* y) {
    parallelFor(0, rows, [=](std::size_t lo, std::size_t hi) {
        for (std::size_t row = lo; row < hi; ++row) {
            float sum = 0.0f;
            for (std::size_t k = rowStart[row]; k < rowStart[row + 1]; ++k) {
                sum += value[k] * x[colIndex[k]];
            }
            y[row] = sum;
        }
    });
}

}  // namespace cyber::accel
