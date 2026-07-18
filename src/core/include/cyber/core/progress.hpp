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
    CancelToken() : m_flag(std::make_shared<std::atomic<bool>>(false)) {}

    void requestCancel() const { m_flag->store(true, std::memory_order_relaxed); }
    [[nodiscard]] bool isCancelled() const { return m_flag->load(std::memory_order_relaxed); }

private:
    std::shared_ptr<std::atomic<bool>> m_flag;
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
