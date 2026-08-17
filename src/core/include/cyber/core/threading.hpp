#pragma once

#include <cstddef>

// Host control over the library's worker fan-out.
//
// Every parallel loop in the engine (accel's IBackend::parallelFor and the
// pipeline's per-vertex relax loops) sizes its fan-out from
// std::thread::hardware_concurrency(). That is the right default for a headless
// batch run and the wrong one inside an interactive app: an AO bake saturating
// every core fights the host's own scheduler and drains a tablet's battery,
// and the host cannot fix it after the fact because the count is read at the
// moment the loop starts.
//
// The knob is an API rather than an environment variable on purpose: a DCC or
// app host is already running when it learns how much of the machine it may
// use, and setenv() in a live multi-threaded process is not safe.
//
// Chunk COUNT is not observable in any result: forEachChunk splits a range into
// contiguous chunks and every primitive built on it writes only the indices it
// was handed, so a capped run is byte-identical to an uncapped one. Only the
// wall-clock and the CPU load change.
namespace cyber {

// Caps the worker threads any one parallel loop may fan out to. 0 (the default)
// means "no cap": the loops keep using hardware_concurrency(). Safe to call
// from any thread at any time; loops already running keep the fan-out they
// started with.
void setMaxWorkerThreads(std::size_t maxThreads);

// The current cap, or 0 when none is set.
[[nodiscard]] std::size_t maxWorkerThreads();

// Workers a loop over `total` items should use: the cap when one is set,
// hardware_concurrency() otherwise, never more than `total` and never 0.
[[nodiscard]] std::size_t workerThreadsFor(std::size_t total);

}  // namespace cyber
