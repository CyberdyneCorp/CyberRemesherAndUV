#include "cyber/core/threading.hpp"

#include <algorithm>
#include <atomic>
#include <thread>

namespace cyber {

namespace {

// Relaxed ordering is enough: the cap is a scheduling hint read once per loop,
// and no other state is published through it.
std::atomic<std::size_t>& capStorage() {
    static std::atomic<std::size_t> cap{0};
    return cap;
}

}  // namespace

void setMaxWorkerThreads(std::size_t maxThreads) {
    capStorage().store(maxThreads, std::memory_order_relaxed);
}

std::size_t maxWorkerThreads() { return capStorage().load(std::memory_order_relaxed); }

std::size_t workerThreadsFor(std::size_t total) {
    const std::size_t cap = maxWorkerThreads();
    const std::size_t budget =
        cap > 0 ? cap : std::max<std::size_t>(1, std::thread::hardware_concurrency());
    return std::max<std::size_t>(1, std::min(budget, total));
}

}  // namespace cyber
