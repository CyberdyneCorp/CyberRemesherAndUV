#pragma once

#include <array>
#include <cstdint>
#include <span>
#include <unordered_map>

#include "cyber/core/math.hpp"

// Interactive viewport camera and touch-gesture recognition (viewport-rendering
// spec, task 7.5). Entirely plain C++ and unit-testable: no GPU state lives
// here, only the math that turns pointer/touch input into a view transform.
namespace cyber::render {

// Column-major 4x4 matrix stored as 16 floats, matching Metal/Vulkan clip
// conventions (right-handed view space, 0..1 depth).
using Mat4 = std::array<float, 16>;

[[nodiscard]] Mat4 identityMatrix();
[[nodiscard]] Mat4 multiply(const Mat4& a, const Mat4& b);
[[nodiscard]] Vec3 transformPoint(const Mat4& m, Vec3 p);

// Right-handed look-at view matrix.
[[nodiscard]] Mat4 lookAt(Vec3 eye, Vec3 target, Vec3 up);
// Perspective projection with 0..1 depth range (fovY in radians).
[[nodiscard]] Mat4 perspective(float fovYRadians, float aspect, float nearZ, float farZ);

// Unit quaternion (x, y, z, w).
struct Quat {
    float x = 0.0f, y = 0.0f, z = 0.0f, w = 1.0f;
};
[[nodiscard]] Quat quatFromAxisAngle(Vec3 axis, float radians);
[[nodiscard]] Quat multiply(Quat a, Quat b);
[[nodiscard]] Quat normalized(Quat q);
[[nodiscard]] Vec3 rotate(Quat q, Vec3 v);

// Orbit / arcball camera around a pivot. Orbiting can run in cheap yaw/pitch
// mode or in full arcball mode (drag-on-a-sphere) that avoids gimbal lock.
class OrbitCamera {
public:
    [[nodiscard]] Vec3 target() const { return m_target; }
    [[nodiscard]] float distance() const { return m_distance; }
    [[nodiscard]] Vec3 eye() const;
    [[nodiscard]] Mat4 viewMatrix() const;
    [[nodiscard]] Mat4 projectionMatrix(float aspect) const;

    void setTarget(Vec3 target) { m_target = target; }
    void setDistance(float distance);
    void setFieldOfView(float fovYRadians) { m_fovY = fovYRadians; }
    void setClipPlanes(float nearZ, float farZ);

    // Frame the pivot at `center` with a distance that fits a sphere of
    // `radius` in view.
    void frameSphere(Vec3 center, float radius);

    // ---- yaw/pitch orbit (radians) ----
    void orbit(float deltaYaw, float deltaPitch);
    // ---- arcball orbit from two normalized screen points in [-1, 1] ----
    void arcball(Vec2 fromNdc, Vec2 toNdc);
    // Screen-space pan; deltas are in normalized viewport units.
    void pan(float deltaX, float deltaY);
    // Multiplicative zoom: factor < 1 moves closer, > 1 moves away.
    void dolly(float factor);

private:
    Vec3 m_target{0.0f, 0.0f, 0.0f};
    float m_distance = 5.0f;
    float m_yaw = 0.0f;    // radians around world up (+Y)
    float m_pitch = 0.0f;  // radians, clamped away from the poles
    Quat m_orientation{};  // arcball accumulator
    bool m_arcballMode = false;
    float m_fovY = 0.9599310886f;  // ~55 degrees
    float m_nearZ = 0.01f;
    float m_farZ = 1000.0f;
};

// ---- touch gestures ---------------------------------------------------------

// A single active contact for one input frame. `majorRadiusPx` is the contact
// ellipse major radius reported by the digitizer; a large value indicates a
// palm/heel rather than a fingertip.
struct TouchPoint {
    std::int32_t id = -1;
    Vec2 position{0.0f, 0.0f};  // pixels
    float majorRadiusPx = 0.0f;
    float pressure = 1.0f;
};

enum class GestureKind : std::uint8_t {
    None,   // ambiguous / rejected — do not move the camera
    Orbit,  // exactly one fingertip dragging
    Pan,    // exactly two fingertips translating together
    Dolly,  // exactly two fingertips pinching
};

struct GestureSample {
    GestureKind kind = GestureKind::None;
    Vec2 delta{0.0f, 0.0f};    // centroid translation in pixels (Orbit/Pan)
    float dollyFactor = 1.0f;  // pinch scale (Dolly); 1 = no change
    int fingertipCount = 0;    // contacts surviving palm rejection
    int rejectedCount = 0;     // contacts discarded as palm/noise
};

struct GestureConfig {
    // Contacts whose major radius exceeds this are treated as a palm.
    float palmRadiusPx = 40.0f;
    // A contact whose radius grows past palmRadiusPx by this factor within one
    // frame is a settling palm and is rejected even if it started small.
    float palmGrowthFactor = 2.5f;
    // Below this two-finger spread change (fraction) motion counts as Pan,
    // otherwise Dolly.
    float dollyThreshold = 0.02f;
    bool strictContactCount = true;  // reject frames with extra contacts
};

// Strict, stateful recognizer. "Strict contact count" means a gesture only
// fires when the number of surviving fingertips exactly matches its arity
// (1 = orbit, 2 = pan/dolly); any other count yields GestureKind::None so a
// resting palm plus a finger never produces spurious camera motion.
class GestureRecognizer {
public:
    GestureRecognizer() = default;
    explicit GestureRecognizer(const GestureConfig& config) : m_config(config) {}

    [[nodiscard]] const GestureConfig& config() const { return m_config; }

    // Feed the current frame's raw contacts; returns the recognized gesture and
    // its delta relative to the previous frame.
    GestureSample update(std::span<const TouchPoint> contacts);
    // Forget all tracked contacts (e.g. on focus loss).
    void reset();

private:
    [[nodiscard]] bool isPalm(const TouchPoint& point) const;

    GestureConfig m_config{};
    std::unordered_map<std::int32_t, Vec2> m_previous;  // fingertip id -> position
    bool m_hasPrevious = false;
};

}  // namespace cyber::render
