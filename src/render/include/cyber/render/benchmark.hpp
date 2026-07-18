#pragma once

// UNVERIFIED: authored best-effort on non-GPU headless hardware. The frame-time
// measurement is real C++, but the performance FLOOR it checks (60 fps at 5 M
// triangles on the reference hardware — viewport-rendering spec 7.5) can only be
// exercised with a live GPU swapchain, which is not present in CI. Needs a first
// run on the reference device.

#include <algorithm>
#include <cstddef>
#include <vector>

namespace cyber::render {

// Performance floors the viewport must meet (viewport-rendering spec 7.5).
struct PerfFloor {
    double minFps = 60.0;
    std::size_t referenceTriangles = 5'000'000;
};

// Rolling frame-time statistics gathered by the viewport benchmark. `record`
// is fed each presented frame's duration; the summary compares against the
// floor. This half is real and testable; wiring it to a GPU swapchain is 7.5's
// unverified part.
class FrameTimer {
public:
    void record(double frameSeconds) {
        m_frames.push_back(frameSeconds);
        m_total += frameSeconds;
    }
    [[nodiscard]] std::size_t frames() const { return m_frames.size(); }
    [[nodiscard]] double averageFps() const {
        return m_total > 0.0 ? static_cast<double>(m_frames.size()) / m_total : 0.0;
    }
    // 1st-percentile FPS (worst frames) — what actually determines smoothness.
    [[nodiscard]] double onePercentLowFps() const {
        if (m_frames.empty()) {
            return 0.0;
        }
        std::vector<double> sorted = m_frames;
        std::sort(sorted.begin(), sorted.end());  // slowest frames last
        const std::size_t idx = (sorted.size() * 99) / 100;
        const double worst = sorted[std::min(idx, sorted.size() - 1)];
        return worst > 0.0 ? 1.0 / worst : 0.0;
    }
    [[nodiscard]] bool meetsFloor(const PerfFloor& floor) const {
        return onePercentLowFps() >= floor.minFps;
    }

private:
    std::vector<double> m_frames;
    double m_total = 0.0;
};

}  // namespace cyber::render
