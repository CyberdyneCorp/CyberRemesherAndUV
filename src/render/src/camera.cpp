#include "cyber/render/camera.hpp"

#include <algorithm>
#include <cmath>
#include <vector>

namespace cyber::render {

// ---- matrix helpers ---------------------------------------------------------

Mat4 identityMatrix() {
    Mat4 m{};
    m[0] = 1.0f;
    m[5] = 1.0f;
    m[10] = 1.0f;
    m[15] = 1.0f;
    return m;
}

Mat4 multiply(const Mat4& a, const Mat4& b) {
    // Column-major: result column j = a * (column j of b).
    Mat4 r{};
    for (int col = 0; col < 4; ++col) {
        for (int row = 0; row < 4; ++row) {
            float sum = 0.0f;
            for (int k = 0; k < 4; ++k) {
                sum += a[static_cast<std::size_t>(k * 4 + row)] *
                       b[static_cast<std::size_t>(col * 4 + k)];
            }
            r[static_cast<std::size_t>(col * 4 + row)] = sum;
        }
    }
    return r;
}

Vec3 transformPoint(const Mat4& m, Vec3 p) {
    const float x = m[0] * p.x + m[4] * p.y + m[8] * p.z + m[12];
    const float y = m[1] * p.x + m[5] * p.y + m[9] * p.z + m[13];
    const float z = m[2] * p.x + m[6] * p.y + m[10] * p.z + m[14];
    const float w = m[3] * p.x + m[7] * p.y + m[11] * p.z + m[15];
    const float inv = (w != 0.0f) ? 1.0f / w : 1.0f;
    return {x * inv, y * inv, z * inv};
}

Mat4 lookAt(Vec3 eye, Vec3 target, Vec3 up) {
    const Vec3 f = normalized(target - eye);      // forward (toward target)
    const Vec3 s = normalized(cross(f, up));      // right
    const Vec3 u = cross(s, f);                   // recomputed up
    Mat4 m = identityMatrix();
    m[0] = s.x;
    m[4] = s.y;
    m[8] = s.z;
    m[1] = u.x;
    m[5] = u.y;
    m[9] = u.z;
    m[2] = -f.x;
    m[6] = -f.y;
    m[10] = -f.z;
    m[12] = -dot(s, eye);
    m[13] = -dot(u, eye);
    m[14] = dot(f, eye);
    return m;
}

Mat4 perspective(float fovYRadians, float aspect, float nearZ, float farZ) {
    const float t = std::tan(fovYRadians * 0.5f);
    const float invT = (t != 0.0f) ? 1.0f / t : 0.0f;
    const float invAspect = (aspect != 0.0f) ? 1.0f / aspect : 0.0f;
    Mat4 m{};
    m[0] = invT * invAspect;
    m[5] = invT;
    m[10] = farZ / (nearZ - farZ);       // 0..1 depth range
    m[11] = -1.0f;
    m[14] = (farZ * nearZ) / (nearZ - farZ);
    return m;
}

// ---- quaternion helpers -----------------------------------------------------

Quat quatFromAxisAngle(Vec3 axis, float radians) {
    const Vec3 n = normalized(axis);
    const float half = radians * 0.5f;
    const float s = std::sin(half);
    return {n.x * s, n.y * s, n.z * s, std::cos(half)};
}

Quat multiply(Quat a, Quat b) {
    return {
        a.w * b.x + a.x * b.w + a.y * b.z - a.z * b.y,
        a.w * b.y - a.x * b.z + a.y * b.w + a.z * b.x,
        a.w * b.z + a.x * b.y - a.y * b.x + a.z * b.w,
        a.w * b.w - a.x * b.x - a.y * b.y - a.z * b.z,
    };
}

Quat normalized(Quat q) {
    const float len = std::sqrt(q.x * q.x + q.y * q.y + q.z * q.z + q.w * q.w);
    if (len <= 0.0f) {
        return Quat{};
    }
    const float inv = 1.0f / len;
    return {q.x * inv, q.y * inv, q.z * inv, q.w * inv};
}

Vec3 rotate(Quat q, Vec3 v) {
    // v' = v + 2 * cross(q.xyz, cross(q.xyz, v) + q.w * v)
    const Vec3 u{q.x, q.y, q.z};
    const Vec3 t = cross(u, v) + v * q.w;
    return v + cross(u, t) * 2.0f;
}

// ---- OrbitCamera ------------------------------------------------------------

namespace {
constexpr float kMinDistance = 1e-3f;
constexpr float kPitchLimit = 1.55334303f;  // ~89 degrees, keeps off the poles
}  // namespace

void OrbitCamera::setDistance(float distance) { m_distance = std::max(distance, kMinDistance); }

void OrbitCamera::setClipPlanes(float nearZ, float farZ) {
    m_nearZ = std::max(nearZ, 1e-4f);
    m_farZ = std::max(farZ, m_nearZ * 2.0f);
}

void OrbitCamera::frameSphere(Vec3 center, float radius) {
    m_target = center;
    const float safeRadius = std::max(radius, kMinDistance);
    const float halfFov = std::max(m_fovY * 0.5f, 1e-3f);
    setDistance(safeRadius / std::sin(halfFov));
    setClipPlanes(m_distance * 0.01f, m_distance + safeRadius * 4.0f);
}

Vec3 OrbitCamera::eye() const {
    Vec3 offset;
    if (m_arcballMode) {
        offset = rotate(m_orientation, Vec3{0.0f, 0.0f, m_distance});
    } else {
        const float cp = std::cos(m_pitch);
        offset = Vec3{std::sin(m_yaw) * cp, std::sin(m_pitch), std::cos(m_yaw) * cp} * m_distance;
    }
    return m_target + offset;
}

Mat4 OrbitCamera::viewMatrix() const { return lookAt(eye(), m_target, Vec3{0.0f, 1.0f, 0.0f}); }

Mat4 OrbitCamera::projectionMatrix(float aspect) const {
    return perspective(m_fovY, aspect, m_nearZ, m_farZ);
}

void OrbitCamera::orbit(float deltaYaw, float deltaPitch) {
    m_arcballMode = false;
    m_yaw += deltaYaw;
    m_pitch = std::clamp(m_pitch + deltaPitch, -kPitchLimit, kPitchLimit);
}

void OrbitCamera::arcball(Vec2 fromNdc, Vec2 toNdc) {
    // Map each screen point onto the arcball sphere (Shoemake).
    auto onSphere = [](Vec2 p) -> Vec3 {
        const float d2 = p.x * p.x + p.y * p.y;
        if (d2 <= 1.0f) {
            return {p.x, p.y, std::sqrt(1.0f - d2)};
        }
        const float inv = 1.0f / std::sqrt(d2);
        return {p.x * inv, p.y * inv, 0.0f};
    };
    const Vec3 a = onSphere(fromNdc);
    const Vec3 b = onSphere(toNdc);
    const Vec3 axis = cross(a, b);
    const float axisLen = length(axis);
    if (axisLen <= 1e-6f) {
        return;
    }
    const float angle = std::acos(std::clamp(dot(a, b), -1.0f, 1.0f));
    m_arcballMode = true;
    m_orientation = normalized(multiply(quatFromAxisAngle(axis, angle), m_orientation));
}

void OrbitCamera::pan(float deltaX, float deltaY) {
    const Vec3 forward = normalized(m_target - eye());
    const Vec3 right = normalized(cross(forward, Vec3{0.0f, 1.0f, 0.0f}));
    const Vec3 up = cross(right, forward);
    // Scale by distance so panning feels constant on screen.
    const float scale = m_distance;
    m_target = m_target + right * (-deltaX * scale) + up * (deltaY * scale);
}

void OrbitCamera::dolly(float factor) { setDistance(m_distance * std::max(factor, 1e-3f)); }

// ---- GestureRecognizer ------------------------------------------------------

bool GestureRecognizer::isPalm(const TouchPoint& point) const {
    if (point.majorRadiusPx >= m_config.palmRadiusPx) {
        return true;
    }
    // A contact settling into a palm may still be growing this frame.
    return point.majorRadiusPx * m_config.palmGrowthFactor >= m_config.palmRadiusPx &&
           point.pressure <= 0.05f;
}

void GestureRecognizer::reset() {
    m_previous.clear();
    m_hasPrevious = false;
}

GestureSample GestureRecognizer::update(std::span<const TouchPoint> contacts) {
    GestureSample sample;

    // Palm rejection: keep only plausible fingertips.
    std::vector<const TouchPoint*> fingers;
    fingers.reserve(contacts.size());
    for (const TouchPoint& c : contacts) {
        if (isPalm(c)) {
            ++sample.rejectedCount;
        } else {
            fingers.push_back(&c);
        }
    }
    sample.fingertipCount = static_cast<int>(fingers.size());

    // Build the current position map for continuity next frame.
    std::unordered_map<std::int32_t, Vec2> current;
    current.reserve(fingers.size());
    for (const TouchPoint* f : fingers) {
        current.emplace(f->id, f->position);
    }

    const bool hadPrevious = m_hasPrevious;
    const auto previous = m_previous;  // snapshot for delta computation
    m_previous = current;
    m_hasPrevious = true;

    if (!hadPrevious) {
        return sample;  // first frame of a touch: establish baseline only
    }

    // Strict contact count: every tracked finger must persist from last frame,
    // otherwise the gesture is ambiguous (finger added/lifted) and we bail.
    auto tracked = [&](const TouchPoint* f) { return previous.find(f->id) != previous.end(); };
    const bool allTracked = std::all_of(fingers.begin(), fingers.end(), tracked) &&
                            previous.size() == fingers.size();

    if (m_config.strictContactCount && !allTracked) {
        return sample;
    }

    if (fingers.size() == 1) {
        const Vec2 prev = previous.at(fingers[0]->id);
        sample.kind = GestureKind::Orbit;
        sample.delta = fingers[0]->position - prev;
        return sample;
    }

    if (fingers.size() == 2) {
        const Vec2 p0Prev = previous.at(fingers[0]->id);
        const Vec2 p1Prev = previous.at(fingers[1]->id);
        const Vec2 p0 = fingers[0]->position;
        const Vec2 p1 = fingers[1]->position;

        const Vec2 centroidPrev = (p0Prev + p1Prev) * 0.5f;
        const Vec2 centroid = (p0 + p1) * 0.5f;

        const Vec2 spanPrev = p1Prev - p0Prev;
        const Vec2 span = p1 - p0;
        const float distPrev = std::sqrt(spanPrev.x * spanPrev.x + spanPrev.y * spanPrev.y);
        const float dist = std::sqrt(span.x * span.x + span.y * span.y);

        const float ratio = (distPrev > 1e-3f) ? dist / distPrev : 1.0f;
        if (std::abs(ratio - 1.0f) > m_config.dollyThreshold) {
            sample.kind = GestureKind::Dolly;
            sample.dollyFactor = ratio;
        } else {
            sample.kind = GestureKind::Pan;
            sample.delta = centroid - centroidPrev;
        }
        return sample;
    }

    // Zero or 3+ fingertips: no recognized gesture.
    return sample;
}

}  // namespace cyber::render
