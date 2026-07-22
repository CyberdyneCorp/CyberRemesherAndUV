#pragma once

#include <cstddef>
#include <cstdint>
#include <vector>

#include "cyber/core/math.hpp"

// Shared, GUI-independent input layer (application-shell spec, task 8.3). All
// recognition here is pure logic driven by synthetic timed events, so it is
// fully testable off-device and reused verbatim by every platform shell:
//   - stroke capture (position + pressure + time samples);
//   - chorded-modifier state;
//   - double-tap and press-hold gesture recognizers;
//   - hover tracking.
// Time is in seconds; positions are in logical points.
namespace cyber::app {

using Time = double;

// ---- chorded modifiers ------------------------------------------------
enum class Modifier : std::uint32_t {
    None = 0u,
    Shift = 1u << 0,
    Ctrl = 1u << 1,
    Alt = 1u << 2,
    Meta = 1u << 3,
};

[[nodiscard]] inline std::uint32_t operator|(Modifier a, Modifier b) {
    return static_cast<std::uint32_t>(a) | static_cast<std::uint32_t>(b);
}
[[nodiscard]] inline std::uint32_t operator|(std::uint32_t a, Modifier b) {
    return a | static_cast<std::uint32_t>(b);
}

class ModifierState {
public:
    void press(Modifier m) { m_mask |= static_cast<std::uint32_t>(m); }
    void release(Modifier m) { m_mask &= ~static_cast<std::uint32_t>(m); }
    void set(std::uint32_t mask) { m_mask = mask; }
    void clear() { m_mask = 0u; }

    [[nodiscard]] std::uint32_t mask() const { return m_mask; }
    [[nodiscard]] bool isDown(Modifier m) const {
        return (m_mask & static_cast<std::uint32_t>(m)) != 0u;
    }
    // True when exactly the chord (and nothing else) is held.
    [[nodiscard]] bool matches(std::uint32_t chord) const { return m_mask == chord; }

private:
    std::uint32_t m_mask = 0u;
};

// ---- stroke capture ---------------------------------------------------
struct StrokeSample {
    Vec2 position;
    float pressure = 1.0f;
    Time time = 0.0;
};

struct Stroke {
    std::vector<StrokeSample> samples;

    [[nodiscard]] bool empty() const { return samples.empty(); }
    [[nodiscard]] std::size_t size() const { return samples.size(); }
    [[nodiscard]] Time duration() const;    // last.time - first.time (0 if <2)
    [[nodiscard]] float arcLength() const;  // summed segment lengths
};

class StrokeCapture {
public:
    void begin(Vec2 position, float pressure, Time time);
    // Adds a sample to the active stroke; ignored when no stroke is active.
    void extend(Vec2 position, float pressure, Time time);
    // Ends the active stroke and returns it; empty stroke if none was active.
    [[nodiscard]] Stroke end(Vec2 position, float pressure, Time time);
    void cancel() {
        m_active = false;
        m_current.samples.clear();
    }

    [[nodiscard]] bool active() const { return m_active; }
    [[nodiscard]] const Stroke& current() const { return m_current; }

private:
    Stroke m_current;
    bool m_active = false;
};

// ---- double-tap recognizer -------------------------------------------
struct DoubleTapConfig {
    Time maxInterval = 0.30;    // max seconds between the two taps
    float maxDistance = 24.0f;  // max points between the two taps
};

class DoubleTapRecognizer {
public:
    DoubleTapRecognizer() = default;
    explicit DoubleTapRecognizer(DoubleTapConfig config) : m_config(config) {}

    // Feeds one tap; returns true when this tap completes a double-tap.
    bool tap(Vec2 position, Time time);
    void reset() { m_hasPending = false; }

private:
    DoubleTapConfig m_config;
    Vec2 m_lastPosition;
    Time m_lastTime = 0.0;
    bool m_hasPending = false;
};

// ---- press-hold recognizer -------------------------------------------
struct PressHoldConfig {
    Time holdDuration = 0.50;  // seconds held before firing
    float moveSlop = 12.0f;    // movement beyond this cancels the hold
};

class PressHoldRecognizer {
public:
    PressHoldRecognizer() = default;
    explicit PressHoldRecognizer(PressHoldConfig config) : m_config(config) {}

    void press(Vec2 position, Time time);
    // Reports pointer motion; returns false and cancels the pending hold when
    // movement exceeds the slop radius.
    bool move(Vec2 position);
    // Advances time; returns true the single moment the hold fires.
    bool update(Time now);
    void release();

    [[nodiscard]] bool pressed() const { return m_pressed; }
    [[nodiscard]] bool fired() const { return m_fired; }

private:
    PressHoldConfig m_config;
    Vec2 m_pressPosition;
    Time m_pressTime = 0.0;
    bool m_pressed = false;
    bool m_fired = false;
};

// ---- hover ------------------------------------------------------------
class HoverState {
public:
    static constexpr std::uint32_t kNoTarget = 0xFFFFFFFFu;

    void update(Vec2 position, std::uint32_t target);
    void clear();

    [[nodiscard]] bool hovering() const { return m_hovering; }
    [[nodiscard]] Vec2 position() const { return m_position; }
    [[nodiscard]] std::uint32_t target() const { return m_target; }
    // True when the last update changed which target is hovered.
    [[nodiscard]] bool targetChanged() const { return m_targetChanged; }

private:
    Vec2 m_position;
    std::uint32_t m_target = kNoTarget;
    bool m_hovering = false;
    bool m_targetChanged = false;
};

}  // namespace cyber::app
