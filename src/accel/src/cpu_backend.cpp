#include <limits>
#include <string>

#include "cyber/accel/backend.hpp"
#include "cyber/core/threading.hpp"

namespace cyber::accel {

namespace {

// The CPU backend is the mandatory reference: it adds nothing to the base
// class, whose primitives (including the host worker pool behind parallelFor)
// already define correct results.
class CpuBackend final : public IBackend {
public:
    [[nodiscard]] BackendKind kind() const override { return BackendKind::Cpu; }

    // Reports the fan-out a loop would actually get, so a host that capped the
    // worker count with cyber::setMaxWorkerThreads sees the cap here rather than
    // the machine's core count. Uncapped this is hardware_concurrency(), exactly
    // as before.
    [[nodiscard]] std::string deviceName() const override {
        const std::size_t threads =
            cyber::workerThreadsFor(std::numeric_limits<std::size_t>::max());
        return "CPU (" + std::to_string(threads) + " threads)";
    }
};

}  // namespace

std::shared_ptr<IBackend> makeCpuBackend() { return std::make_shared<CpuBackend>(); }

}  // namespace cyber::accel
