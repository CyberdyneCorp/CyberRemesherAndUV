#include <doctest.h>

#include <atomic>
#include <cstddef>
#include <mutex>
#include <numeric>
#include <set>
#include <string>
#include <thread>
#include <vector>

#include "cyber/accel/backend.hpp"

namespace {

// Stands in for a device backend: it reports a GPU kind but, like the real
// CUDA/OpenCL/Metal backends now do, leaves parallelFor to the base class. The
// base implementation is the library's only host loop primitive, so a backend
// that replaces it with an inline call single-threads the AO texel loop, the
// solver's per-variable loop and the base-class BVH traversal for the whole
// process.
class DeviceLikeBackend final : public cyber::accel::IBackend {
public:
    [[nodiscard]] cyber::accel::BackendKind kind() const override {
        return cyber::accel::BackendKind::Cuda;
    }
    [[nodiscard]] std::string deviceName() const override { return "device-like"; }
};

std::size_t countDistinctThreads(cyber::accel::IBackend& backend, std::size_t n) {
    std::set<std::thread::id> ids;
    std::mutex mutex;
    backend.parallelFor(0, n, [&ids, &mutex](std::size_t, std::size_t) {
        const std::lock_guard<std::mutex> lock(mutex);
        ids.insert(std::this_thread::get_id());
    });
    return ids.size();
}

}  // namespace

TEST_CASE("CPU backend is always available and last") {
    const auto backends = cyber::accel::availableBackends();
    REQUIRE(!backends.empty());
    REQUIRE(backends.back()->kind() == cyber::accel::BackendKind::Cpu);
}

TEST_CASE("parallelFor covers the full range exactly once") {
    auto backend = cyber::accel::defaultBackend();
    constexpr std::size_t n = 100'000;
    std::vector<std::atomic<int>> hits(n);
    backend->parallelFor(0, n, [&hits](std::size_t lo, std::size_t hi) {
        for (std::size_t i = lo; i < hi; ++i) {
            hits[i].fetch_add(1, std::memory_order_relaxed);
        }
    });
    for (std::size_t i = 0; i < n; ++i) {
        REQUIRE(hits[i].load() == 1);
    }
}

TEST_CASE("parallelFor runs a short range on the calling thread") {
    // Regression: a fan-out per call made the AO bake spawn and join
    // ~hardware_concurrency threads for every 64-ray texel batch, which cost an
    // order of magnitude more than the rays themselves.
    auto backend = cyber::accel::defaultBackend();
    std::vector<std::thread::id> chunkThreads;
    backend->parallelFor(0, 32, [&chunkThreads](std::size_t, std::size_t) {
        chunkThreads.push_back(std::this_thread::get_id());
    });
    REQUIRE(chunkThreads.size() == 1);
    REQUIRE(chunkThreads[0] == std::this_thread::get_id());
}

TEST_CASE("parallelFor nested inside another parallelFor does not fan out again") {
    // Nesting would multiply the thread count: the AO bake casts its rays from
    // inside the texel loop's own parallelFor.
    auto backend = cyber::accel::defaultBackend();
    constexpr std::size_t n = 100'000;
    std::atomic<int> violations{0};
    backend->parallelFor(0, n, [&backend, &violations](std::size_t, std::size_t) {
        const std::thread::id worker = std::this_thread::get_id();
        std::atomic<int> innerChunks{0};
        std::atomic<bool> ranElsewhere{false};
        backend->parallelFor(0, n, [&innerChunks, &ranElsewhere, worker](std::size_t, std::size_t) {
            innerChunks.fetch_add(1, std::memory_order_relaxed);
            if (std::this_thread::get_id() != worker) {
                ranElsewhere.store(true, std::memory_order_relaxed);
            }
        });
        if (innerChunks.load() != 1 || ranElsewhere.load()) {
            violations.fetch_add(1, std::memory_order_relaxed);
        }
    });
    REQUIRE(violations.load() == 0);
}

TEST_CASE("a short outer range still suppresses the nested fan-out") {
    // Regression: the serial fast path ran the body without arming the nesting
    // guard, so an outer range below the threshold let EVERY nested parallelFor
    // spawn and join ~hardware_concurrency threads per item — the exact thread
    // storm the guard exists to prevent, and one that made a sub-64-texel AO
    // bake slower in wall time than a larger one.
    auto backend = cyber::accel::defaultBackend();
    constexpr std::size_t shortOuter = 32;  // below the fan-out threshold
    std::atomic<int> violations{0};
    backend->parallelFor(0, shortOuter, [&backend, &violations](std::size_t lo, std::size_t hi) {
        for (std::size_t i = lo; i < hi; ++i) {
            const std::thread::id caller = std::this_thread::get_id();
            std::atomic<int> innerChunks{0};
            std::atomic<bool> ranElsewhere{false};
            backend->parallelFor(0, 100'000,
                                 [&innerChunks, &ranElsewhere, caller](std::size_t, std::size_t) {
                                     innerChunks.fetch_add(1, std::memory_order_relaxed);
                                     if (std::this_thread::get_id() != caller) {
                                         ranElsewhere.store(true, std::memory_order_relaxed);
                                     }
                                 });
            if (innerChunks.load() != 1 || ranElsewhere.load()) {
                violations.fetch_add(1, std::memory_order_relaxed);
            }
        }
    });
    REQUIRE(violations.load() == 0);
}

TEST_CASE("the nesting guard is restored after a nested parallelFor returns") {
    // The guard is scoped, not sticky: a serial inner call must not leave the
    // calling thread marked as "inside a parallel region" for everything after.
    auto backend = cyber::accel::defaultBackend();
    backend->parallelFor(0, 8, [](std::size_t, std::size_t) {});
    REQUIRE(countDistinctThreads(*backend, 100'000) > 1);
}

TEST_CASE("a device-like backend keeps the host worker threads") {
    // Regression: every GPU backend overrode parallelFor to run the whole range
    // inline, so selecting one (registry.cpp does it automatically) made every
    // CPU-side parallel loop in the library serial — the AO texel loop, the
    // solver's integer-variable loop, and the base-class BVH traversal.
    if (std::thread::hardware_concurrency() < 2) {
        return;  // nothing to fan out to
    }
    DeviceLikeBackend backend;
    REQUIRE(countDistinctThreads(backend, 100'000) > 1);
}

TEST_CASE("parallelFor with empty range is a no-op") {
    auto backend = cyber::accel::defaultBackend();
    bool called = false;
    backend->parallelFor(5, 5, [&called](std::size_t, std::size_t) { called = true; });
    REQUIRE(!called);
}
