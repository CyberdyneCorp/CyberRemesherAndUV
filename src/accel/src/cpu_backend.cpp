#include <thread>

#include "cyber/accel/backend.hpp"

namespace cyber::accel {

namespace {

// The CPU backend is the mandatory reference: it adds nothing to the base
// class, whose primitives (including the host worker pool behind parallelFor)
// already define correct results.
class CpuBackend final : public IBackend {
public:
    [[nodiscard]] BackendKind kind() const override { return BackendKind::Cpu; }

    [[nodiscard]] std::string deviceName() const override {
        return "CPU (" + std::to_string(std::thread::hardware_concurrency()) + " threads)";
    }
};

}  // namespace

std::shared_ptr<IBackend> makeCpuBackend() { return std::make_shared<CpuBackend>(); }

}  // namespace cyber::accel
