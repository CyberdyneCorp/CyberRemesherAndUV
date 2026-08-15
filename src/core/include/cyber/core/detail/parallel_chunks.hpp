#pragma once

#include <algorithm>
#include <cstddef>
#include <exception>
#include <mutex>
#include <thread>
#include <utility>
#include <vector>

// Host worker fan-out shared by the library's two parallel loops
// (accel's IBackend::parallelFor and the pipeline's per-vertex relax loops).
//
// It exists for the two failure modes a bare `emplace_back` + `join` loop gets
// wrong, both of which end the whole process:
//   * an exception escaping a worker reaches the default terminate handler, so
//     the C ABI's ~40 catch blocks never see it and the embedding host dies
//     instead of receiving a CyberStatus;
//   * a throw from the spawn loop itself (std::thread runs out of threads, or
//     the vector cannot grow) unwinds over joinable threads, which is also
//     terminate.
// Here a worker's exception is captured, every thread is joined, and the first
// exception is rethrown on the calling thread; a spawn failure stops the
// fan-out and the remaining chunks run inline, so a thread-starved process
// keeps making progress instead of failing.
namespace cyber::detail {

// Runs fn(lo, hi) over [begin, end) split into `workers` contiguous chunks.
// The chunk bounds are exactly those of a plain (total + workers - 1) / workers
// split, and every chunk runs to completion, so results are unchanged by how
// many of them ended up inline.
template <class Fn>
void forEachChunk(std::size_t begin, std::size_t end, std::size_t workers, const Fn& fn) {
    if (begin >= end) {
        return;
    }
    if (workers <= 1) {
        fn(begin, end);
        return;
    }
    const std::size_t chunk = (end - begin + workers - 1) / workers;

    std::mutex mutex;
    std::exception_ptr firstError;
    // noexcept: nothing inside a worker may escape into std::thread's body.
    const auto runChunk = [&fn, &mutex, &firstError](std::size_t lo, std::size_t hi) noexcept {
        try {
            fn(lo, hi);
        } catch (...) {
            try {
                const std::lock_guard<std::mutex> lock(mutex);
                if (!firstError) {
                    firstError = std::current_exception();
                }
            } catch (...) {  // NOLINT — a failing lock must not terminate either
            }
        }
    };

    std::vector<std::thread> threads;
    std::size_t next = begin;
    try {
        threads.reserve(workers);
        for (; next < end; next += chunk) {
            const std::size_t hi = std::min(end, next + chunk);
            threads.emplace_back([&runChunk, lo = next, hi] { runChunk(lo, hi); });
        }
    } catch (...) {
        // Out of threads (or out of memory): the chunks that were not spawned
        // run below on this thread. `next` still points at the first of them.
    }
    for (; next < end; next += chunk) {
        runChunk(next, std::min(end, next + chunk));
    }
    for (std::thread& worker : threads) {
        worker.join();
    }
    if (firstError) {
        std::rethrow_exception(firstError);
    }
}

}  // namespace cyber::detail
