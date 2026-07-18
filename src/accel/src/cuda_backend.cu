// CUDA compute backend (compute-acceleration spec, tier-1 on NVIDIA).
// makeCudaBackend() returns nullptr when no CUDA device is present, so the
// registry degrades gracefully to CPU. Compiled to PTX (virtual arch) so the
// driver JITs for the actual GPU at load time — one build runs across NVIDIA
// generations, including those newer than the build-time toolkit.
//
// Verified on an RTX 5060 (compute cap 12.0, driver 580) with the CUDA
// toolkit's forward PTX JIT, via tests/accel/test_gpu_parity.cpp built with
// -DCYBER_ENABLE_CUDA=ON.

#include <cuda_runtime.h>

#include <memory>
#include <string>
#include <vector>

#include "cyber/accel/backend.hpp"

namespace cyber::accel {

namespace {

__global__ void axpyKernel(float alpha, const float* x, float* y, std::size_t n) {
    const std::size_t i = blockIdx.x * blockDim.x + threadIdx.x;
    if (i < n) {
        y[i] += alpha * x[i];
    }
}

__global__ void spmvKernel(std::size_t rows, const std::size_t* rowStart,
                           const std::size_t* colIndex, const float* value, const float* x,
                           float* y) {
    const std::size_t row = blockIdx.x * blockDim.x + threadIdx.x;
    if (row >= rows) {
        return;
    }
    float sum = 0.0f;
    for (std::size_t k = rowStart[row]; k < rowStart[row + 1]; ++k) {
        sum += value[k] * x[colIndex[k]];
    }
    y[row] = sum;
}

// Minimal owning device-pointer helper so kernel launches stay exception-safe.
template <class T>
class DevicePtr {
public:
    explicit DevicePtr(std::size_t count) { cudaMalloc(&m_ptr, count * sizeof(T)); }
    ~DevicePtr() { cudaFree(m_ptr); }
    DevicePtr(const DevicePtr&) = delete;
    DevicePtr& operator=(const DevicePtr&) = delete;
    [[nodiscard]] T* get() const { return m_ptr; }

private:
    T* m_ptr = nullptr;
};

class CudaBackend final : public IBackend {
public:
    explicit CudaBackend(std::string name) : m_name(std::move(name)) {}

    [[nodiscard]] BackendKind kind() const override { return BackendKind::Cuda; }
    [[nodiscard]] std::string deviceName() const override { return "CUDA (" + m_name + ")"; }

    // Host callables cannot cross to the device; generic parallelFor stays on
    // the host. Accelerated work runs through the typed kernels below.
    void parallelFor(std::size_t begin, std::size_t end,
                     const std::function<void(std::size_t, std::size_t)>& fn) override {
        if (begin < end) {
            fn(begin, end);
        }
    }

    void axpy(float alpha, const float* x, float* y, std::size_t n) override {
        if (n == 0) {
            return;
        }
        DevicePtr<float> dx(n), dy(n);
        cudaMemcpy(dx.get(), x, n * sizeof(float), cudaMemcpyHostToDevice);
        cudaMemcpy(dy.get(), y, n * sizeof(float), cudaMemcpyHostToDevice);
        const unsigned block = 256;
        const unsigned grid = static_cast<unsigned>((n + block - 1) / block);
        axpyKernel<<<grid, block>>>(alpha, dx.get(), dy.get(), n);
        cudaMemcpy(y, dy.get(), n * sizeof(float), cudaMemcpyDeviceToHost);
    }

    void spmvCsr(std::size_t rows, const std::size_t* rowStart, const std::size_t* colIndex,
                 const float* value, const float* x, float* y) override {
        if (rows == 0) {
            return;
        }
        const std::size_t nnz = rowStart[rows];
        DevicePtr<std::size_t> dRowStart(rows + 1), dColIndex(nnz);
        DevicePtr<float> dValue(nnz), dx(rows), dy(rows);
        cudaMemcpy(dRowStart.get(), rowStart, (rows + 1) * sizeof(std::size_t),
                   cudaMemcpyHostToDevice);
        cudaMemcpy(dColIndex.get(), colIndex, nnz * sizeof(std::size_t), cudaMemcpyHostToDevice);
        cudaMemcpy(dValue.get(), value, nnz * sizeof(float), cudaMemcpyHostToDevice);
        cudaMemcpy(dx.get(), x, rows * sizeof(float), cudaMemcpyHostToDevice);
        const unsigned block = 256;
        const unsigned grid = static_cast<unsigned>((rows + block - 1) / block);
        spmvKernel<<<grid, block>>>(rows, dRowStart.get(), dColIndex.get(), dValue.get(), dx.get(),
                                    dy.get());
        cudaMemcpy(y, dy.get(), rows * sizeof(float), cudaMemcpyDeviceToHost);
    }

private:
    std::string m_name;
};

}  // namespace

std::shared_ptr<IBackend> makeCudaBackend() {
    int count = 0;
    if (cudaGetDeviceCount(&count) != cudaSuccess || count == 0) {
        return nullptr;
    }
    cudaDeviceProp props{};
    if (cudaGetDeviceProperties(&props, 0) != cudaSuccess) {
        return nullptr;
    }
    return std::make_shared<CudaBackend>(props.name);
}

}  // namespace cyber::accel
