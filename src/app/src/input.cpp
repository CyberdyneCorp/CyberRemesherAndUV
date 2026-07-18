#include "cyber/app/input.hpp"

#include <cmath>

namespace cyber::app {
namespace {

[[nodiscard]] float distanceSquared(Vec2 a, Vec2 b) {
    const Vec2 d = a - b;
    return d.x * d.x + d.y * d.y;
}

[[nodiscard]] float distance(Vec2 a, Vec2 b) { return std::sqrt(distanceSquared(a, b)); }

}  // namespace

// ---- Stroke -----------------------------------------------------------
Time Stroke::duration() const {
    if (samples.size() < 2) {
        return 0.0;
    }
    return samples.back().time - samples.front().time;
}

float Stroke::arcLength() const {
    float total = 0.0f;
    for (std::size_t i = 1; i < samples.size(); ++i) {
        total += distance(samples[i - 1].position, samples[i].position);
    }
    return total;
}

// ---- StrokeCapture ----------------------------------------------------
void StrokeCapture::begin(Vec2 position, float pressure, Time time) {
    m_current.samples.clear();
    m_current.samples.push_back(StrokeSample{position, pressure, time});
    m_active = true;
}

void StrokeCapture::extend(Vec2 position, float pressure, Time time) {
    if (!m_active) {
        return;
    }
    m_current.samples.push_back(StrokeSample{position, pressure, time});
}

Stroke StrokeCapture::end(Vec2 position, float pressure, Time time) {
    if (!m_active) {
        return Stroke{};
    }
    m_current.samples.push_back(StrokeSample{position, pressure, time});
    m_active = false;
    return m_current;
}

// ---- DoubleTapRecognizer ---------------------------------------------
bool DoubleTapRecognizer::tap(Vec2 position, Time time) {
    if (m_hasPending && (time - m_lastTime) <= m_config.maxInterval &&
        distance(position, m_lastPosition) <= m_config.maxDistance) {
        m_hasPending = false;  // consume the pair
        return true;
    }
    m_lastPosition = position;
    m_lastTime = time;
    m_hasPending = true;
    return false;
}

// ---- PressHoldRecognizer ---------------------------------------------
void PressHoldRecognizer::press(Vec2 position, Time time) {
    m_pressPosition = position;
    m_pressTime = time;
    m_pressed = true;
    m_fired = false;
}

bool PressHoldRecognizer::move(Vec2 position) {
    if (!m_pressed) {
        return false;
    }
    if (distance(position, m_pressPosition) > m_config.moveSlop) {
        m_pressed = false;  // moved too far: no hold
        return false;
    }
    return true;
}

bool PressHoldRecognizer::update(Time now) {
    if (!m_pressed || m_fired) {
        return false;
    }
    if ((now - m_pressTime) >= m_config.holdDuration) {
        m_fired = true;
        return true;
    }
    return false;
}

void PressHoldRecognizer::release() {
    m_pressed = false;
    m_fired = false;
}

// ---- HoverState -------------------------------------------------------
void HoverState::update(Vec2 position, std::uint32_t target) {
    m_targetChanged = !m_hovering || target != m_target;
    m_position = position;
    m_target = target;
    m_hovering = true;
}

void HoverState::clear() {
    m_targetChanged = m_hovering && m_target != kNoTarget;
    m_hovering = false;
    m_target = kNoTarget;
}

}  // namespace cyber::app
