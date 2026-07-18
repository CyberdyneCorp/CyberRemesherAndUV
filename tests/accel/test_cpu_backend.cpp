#include <doctest.h>

#include <atomic>
#include <cstddef>
#include <numeric>
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

TEST_CASE("parallelFor with empty range is a no-op") {
    auto backend = cyber::accel::defaultBackend();
    bool called = false;
    backend->parallelFor(5, 5, [&called](std::size_t, std::size_t) { called = true; });
    REQUIRE(!called);
}
