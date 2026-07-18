#pragma once

#include <cstddef>
#include <functional>
#include <memory>
#include <string>
#include <vector>

namespace cyber::accel {

enum class BackendKind { Cpu, Metal, Cuda, OpenCl };

// Compute backend contract. The CPU backend is always present and defines
// correct results; GPU backends are optional accelerators (see the
// compute-acceleration spec). The primitive set grows with task group 4;
// parallel_for is the seed primitive the isotropic remesher needs first.
class IBackend {
public:
    virtual ~IBackend() = default;

    [[nodiscard]] virtual BackendKind kind() const = 0;
    [[nodiscard]] virtual std::string deviceName() const = 0;

    // Invokes fn(begin..end) partitioned across workers. Blocks until done.
    virtual void parallelFor(std::size_t begin, std::size_t end,
                             const std::function<void(std::size_t, std::size_t)>& fn) = 0;

    // Accelerated numeric primitives (compute-acceleration spec "hot spots":
    // solver matrix-vector products and the dense vector ops around them).
    // The base class provides the CPU reference in terms of parallelFor and
    // defines correct results; GPU backends override with device kernels and
    // are held to these by the parity tests (task 4.6). They take host
    // pointers — a GPU backend stages its own device copies — so the call
    // sites never change when a GPU backend is selected.

    // y[i] += alpha * x[i], for i in [0, n).
    virtual void axpy(float alpha, const float* x, float* y, std::size_t n);
    // Sum of x[i] * y[i].
    [[nodiscard]] virtual float dot(const float* x, const float* y, std::size_t n);
    // y = A * x for a CSR matrix A with `rows` rows (rowStart has rows + 1
    // entries; colIndex/value have nnz entries).
    virtual void spmvCsr(std::size_t rows, const std::size_t* rowStart,
                         const std::size_t* colIndex, const float* value, const float* x,
                         float* y);
};

// Enumerates available backends, best first (Metal/CUDA > OpenCL > CPU).
// The CPU backend is always the last entry.
[[nodiscard]] std::vector<std::shared_ptr<IBackend>> availableBackends();

// The process-wide default backend (first of availableBackends unless
// overridden by setDefaultBackend).
[[nodiscard]] std::shared_ptr<IBackend> defaultBackend();
void setDefaultBackend(std::shared_ptr<IBackend> backend);

// User selection/override (compute-acceleration spec, "Runtime detection,
// selection, and fallback"): return the first available backend of `kind`,
// or the CPU backend when that kind is absent — never null. Passing the
// result to setDefaultBackend makes it process-wide.
[[nodiscard]] std::shared_ptr<IBackend> selectBackend(BackendKind kind);

}  // namespace cyber::accel
