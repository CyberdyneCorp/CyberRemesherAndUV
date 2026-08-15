#pragma once

#include <cstddef>
#include <cstdio>
#include <mutex>
#include <set>
#include <string>

// Shared bookkeeping for GPU-to-CPU fallbacks (compute-acceleration spec:
// "fall back to CPU automatically when a GPU backend fails at runtime ...
// never crashing or producing partial results").
//
// Each backend used to hold ONE function-local once_flag for all of its
// primitives, so a first spmv fallback permanently silenced a later, different
// raycast fallback and the operator saw a single line for a run that had spent
// the whole time on the CPU. Announcement is per (backend, primitive) pair
// here, and every event is counted so a host can tell that a run it configured
// for the GPU did not stay there.
namespace cyber::accel::detail {

struct FallbackLog {
    std::mutex mutex;
    std::set<std::string> announced;  // "CUDA spmvCsr", "OpenCL raycastBvh", ...
    std::size_t count = 0;
};

inline FallbackLog& fallbackLog() {
    static FallbackLog log;
    return log;
}

// Counts the event and prints one stderr line the first time this backend's
// `primitive` falls back.
inline void reportFallbackOnce(const char* backend, const char* primitive, const char* what) {
    FallbackLog& log = fallbackLog();
    bool announce = false;
    {
        const std::lock_guard<std::mutex> lock(log.mutex);
        ++log.count;
        announce = log.announced.emplace(std::string(backend) + " " + primitive).second;
    }
    if (announce) {
        std::fprintf(stderr, "[accel] %s %s failed (%s); falling back to the CPU reference\n",
                     backend, primitive, what);
    }
}

[[nodiscard]] inline std::size_t fallbackEventCount() {
    FallbackLog& log = fallbackLog();
    const std::lock_guard<std::mutex> lock(log.mutex);
    return log.count;
}

}  // namespace cyber::accel::detail
