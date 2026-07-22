#pragma once

#include <atomic>
#include <functional>
#include <memory>
#include <string_view>

namespace cyber {

// Cooperative cancellation (remeshing-pipeline spec, "Progress reporting and
// cooperative cancellation"): long-running engine calls poll the token at
// bounded intervals (<= 100 ms worst case) and abort leaving their inputs
// untouched. Copyable; all copies share one flag.
class CancelToken {
public:
    CancelToken()
        : m_flag(std::make_shared<std::atomic<bool>>(false)),
          m_poll(std::make_shared<std::function<bool()>>()) {}

    void requestCancel() const { m_flag->store(true, std::memory_order_relaxed); }

    // Install a poll: isCancelled() invokes it directly (and latches the flag) whenever
    // the flag is not already set. This lets long-running calls that only poll the token
    // — not the progress sink — observe a cancel promptly (the C-API bridge otherwise only
    // flips the flag on a progress report, so a report-less solve could never be cancelled
    // mid-flight). Shared across copies. Callable is polled cheaply on each isCancelled().
    void setPoll(std::function<bool()> poll) const { *m_poll = std::move(poll); }

    [[nodiscard]] bool isCancelled() const {
        if (m_flag->load(std::memory_order_relaxed)) {
            return true;
        }
        if (*m_poll && (*m_poll)()) {
            m_flag->store(true, std::memory_order_relaxed);
            return true;
        }
        return false;
    }

private:
    std::shared_ptr<std::atomic<bool>> m_flag;
    std::shared_ptr<std::function<bool()>> m_poll;
};

// Monotonic progress reporting: values are clamped so the merged progress
// visible to callers never decreases, whatever order workers report in.
class ProgressSink {
public:
    using Callback = std::function<void(float progress, std::string_view stage)>;

    ProgressSink() = default;
    explicit ProgressSink(Callback callback) : m_callback(std::move(callback)) {}

    void report(float progress, std::string_view stage = {}) {
        if (!m_callback) {
            return;
        }
        float current = m_best.load(std::memory_order_relaxed);
        while (progress > current &&
               !m_best.compare_exchange_weak(current, progress, std::memory_order_relaxed)) {
        }
        if (progress >= current) {
            m_callback(m_best.load(std::memory_order_relaxed), stage);
        }
    }

    // A child sink mapping [0,1] into [lo,hi] of this sink.
    [[nodiscard]] ProgressSink subrange(float lo, float hi, std::string_view stage) {
        if (!m_callback) {
            return ProgressSink{};
        }
        return ProgressSink(
            [this, lo, hi, stageName = std::string(stage)](float p, std::string_view) {
                report(lo + (hi - lo) * p, stageName);
            });
    }

private:
    Callback m_callback;
    std::atomic<float> m_best{0.0f};
};

}  // namespace cyber
