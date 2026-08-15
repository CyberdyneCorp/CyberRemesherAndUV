#include <algorithm>
#include <thread>

#include "cyber/accel/backend.hpp"

namespace cyber::accel {

namespace {

// Shortest range worth handing to worker threads at all. Spawning and joining
// std::threads costs far more than a loop of a few dozen items — the failure
// mode IBackend::spmvCsr already guards against with its nnz threshold. Kept
// deliberately low: parallelFor cannot know the per-item cost, and a caller
// whose items are expensive must not silently lose its workers. Chunking is
// never observable either way — every primitive built on parallelFor writes only
// the indices it was handed, so a serial run is byte-identical to a threaded
// one.
constexpr std::size_t kMinParallelItems = 64;

// True while this thread is executing a parallelFor chunk. A parallelFor invoked
// from inside another one runs inline instead of fanning out again: nesting
// would multiply the thread count (the AO bake casts its rays from inside the
// texel loop's own parallelFor).
thread_local bool tInParallelRegion = false;

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
        const std::size_t workers = workerCount(total);
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
            threads.emplace_back([&fn, lo, hi] {
                tInParallelRegion = true;
                fn(lo, hi);
            });
        }
        for (auto& t : threads) {
            t.join();
        }
    }

private:
    static std::size_t workerCount(std::size_t total) {
        if (tInParallelRegion || total < kMinParallelItems) {
            return 1;
        }
        const std::size_t hw = std::max<std::size_t>(1, std::thread::hardware_concurrency());
        return std::min(hw, total);
    }
};

}  // namespace

std::shared_ptr<IBackend> makeCpuBackend() { return std::make_shared<CpuBackend>(); }

}  // namespace cyber::accel
