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
};

// Enumerates available backends, best first (Metal/CUDA > OpenCL > CPU).
// The CPU backend is always the last entry.
[[nodiscard]] std::vector<std::shared_ptr<IBackend>> availableBackends();

// The process-wide default backend (first of availableBackends unless
// overridden by setDefaultBackend).
[[nodiscard]] std::shared_ptr<IBackend> defaultBackend();
void setDefaultBackend(std::shared_ptr<IBackend> backend);

}  // namespace cyber::accel
