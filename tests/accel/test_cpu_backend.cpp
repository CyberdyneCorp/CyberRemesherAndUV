#include <doctest.h>

#include <atomic>
#include <cstddef>
#include <numeric>
#include <thread>
#include <vector>

#include "cyber/accel/backend.hpp"

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

TEST_CASE("parallelFor with empty range is a no-op") {
    auto backend = cyber::accel::defaultBackend();
    bool called = false;
    backend->parallelFor(5, 5, [&called](std::size_t, std::size_t) { called = true; });
    REQUIRE(!called);
}
