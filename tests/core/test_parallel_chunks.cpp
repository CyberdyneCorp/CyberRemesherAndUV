#include <doctest.h>

#include <algorithm>
#include <atomic>
#include <cstddef>
#include <mutex>
#include <stdexcept>
#include <thread>
#include <utility>
#include <vector>

#include "cyber/core/detail/parallel_chunks.hpp"

// The host worker fan-out behind IBackend::parallelFor and the pipeline's
// per-vertex relax loops. Its contract is (1) the partition is exactly the plain
// ceil split, so threaded and serial runs stay byte-identical, and (2) an
// exception from a chunk never escapes a worker thread — it is rethrown on the
// caller, after every worker has been joined, so the C ABI's handlers can turn
// it into a CyberStatus instead of the process terminating.
namespace {

std::vector<std::pair<std::size_t, std::size_t>> collectChunks(std::size_t begin, std::size_t end,
                                                               std::size_t workers) {
    std::mutex mutex;
    std::vector<std::pair<std::size_t, std::size_t>> chunks;
    cyber::detail::forEachChunk(begin, end, workers, [&](std::size_t lo, std::size_t hi) {
        const std::lock_guard<std::mutex> lock(mutex);
        chunks.emplace_back(lo, hi);
    });
    std::sort(chunks.begin(), chunks.end());
    return chunks;
}

}  // namespace

TEST_CASE("chunks tile the range exactly once") {
    const auto chunks = collectChunks(0, 1000, 7);
    REQUIRE(!chunks.empty());
    REQUIRE(chunks.front().first == 0);
    REQUIRE(chunks.back().second == 1000);
    for (std::size_t i = 1; i < chunks.size(); ++i) {
        REQUIRE(chunks[i].first == chunks[i - 1].second);
    }
    // ceil(1000 / 7) = 143 per chunk, so 7 chunks with a short tail.
    REQUIRE(chunks.size() == 7);
    REQUIRE(chunks.front().second == 143);

    REQUIRE(collectChunks(0, 0, 4).empty());
    REQUIRE(collectChunks(10, 10, 4).empty());
    const auto serial = collectChunks(5, 25, 1);
    REQUIRE(serial.size() == 1);
    REQUIRE(serial.front() == std::pair<std::size_t, std::size_t>{5, 25});
}

TEST_CASE("an exception from a chunk is rethrown on the calling thread") {
    // The failure this pins: an unguarded worker body reaches the default
    // terminate handler, killing the embedding host instead of returning a
    // status. Every other chunk still completes, and the workers are all joined
    // before the throw reaches the caller — a throw with a joinable thread still
    // outstanding is also terminate.
    constexpr std::size_t n = 10'000;
    std::atomic<std::size_t> visited{0};
    const std::thread::id caller = std::this_thread::get_id();
    std::atomic<bool> sawWorker{false};

    const auto body = [&](std::size_t lo, std::size_t hi) {
        if (std::this_thread::get_id() != caller) {
            sawWorker.store(true, std::memory_order_relaxed);
        }
        visited.fetch_add(hi - lo, std::memory_order_relaxed);
        if (lo == 0) {
            throw std::runtime_error("chunk failure");
        }
    };
    REQUIRE_THROWS_AS(cyber::detail::forEachChunk(0, n, 8, body), std::runtime_error);
    REQUIRE(visited.load() == n);
    if (std::thread::hardware_concurrency() >= 2) {
        REQUIRE(sawWorker.load());
    }
}

TEST_CASE("the first exception wins when several chunks throw") {
    // All of them are swallowed except one; none of them escapes a worker.
    std::atomic<std::size_t> thrown{0};
    const auto body = [&thrown](std::size_t, std::size_t) {
        thrown.fetch_add(1, std::memory_order_relaxed);
        throw std::runtime_error("every chunk fails");
    };
    REQUIRE_THROWS_AS(cyber::detail::forEachChunk(0, 800, 8, body), std::runtime_error);
    REQUIRE(thrown.load() == 8);
}
