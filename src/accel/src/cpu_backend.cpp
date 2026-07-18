#include <algorithm>
#include <thread>

#include "cyber/accel/backend.hpp"

namespace cyber::accel {

namespace {

class CpuBackend final : public IBackend {
public:
    [[nodiscard]] BackendKind kind() const override { return BackendKind::Cpu; }

    [[nodiscard]] std::string deviceName() const override {
        return "CPU (" + std::to_string(std::thread::hardware_concurrency()) + " threads)";
    }

    void parallelFor(std::size_t begin, std::size_t end,
                     const std::function<void(std::size_t, std::size_t)>& fn) override {
        if (begin >= end) {
            return;
        }
        const std::size_t total = end - begin;
        const std::size_t hw = std::max<std::size_t>(1, std::thread::hardware_concurrency());
        const std::size_t workers = std::min(hw, total);
        if (workers == 1) {
            fn(begin, end);
            return;
        }
        const std::size_t chunk = (total + workers - 1) / workers;
        std::vector<std::thread> threads;
        threads.reserve(workers);
        for (std::size_t w = 0; w < workers; ++w) {
            const std::size_t lo = begin + w * chunk;
            const std::size_t hi = std::min(end, lo + chunk);
            if (lo >= hi) {
                break;
            }
            threads.emplace_back([&fn, lo, hi] { fn(lo, hi); });
        }
        for (auto& t : threads) {
            t.join();
        }
    }
};

}  // namespace

std::shared_ptr<IBackend> makeCpuBackend() { return std::make_shared<CpuBackend>(); }

}  // namespace cyber::accel
