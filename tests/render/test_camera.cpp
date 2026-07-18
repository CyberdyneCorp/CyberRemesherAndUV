#include <doctest.h>

#include <cmath>
#include <vector>

#include "cyber/render/camera.hpp"

using namespace cyber;
using namespace cyber::render;

namespace {

bool nearlyEqual(Vec3 a, Vec3 b, float eps = 1e-4f) {
    return std::abs(a.x - b.x) < eps && std::abs(a.y - b.y) < eps && std::abs(a.z - b.z) < eps;
}

}  // namespace

TEST_CASE("identity matrix is multiplicative identity") {
    const Mat4 id = identityMatrix();
    const Mat4 m = multiply(id, id);
    for (std::size_t i = 0; i < 16; ++i) {
        CHECK(m[i] == doctest::Approx(id[i]));
    }
}

TEST_CASE("lookAt places the target on the negative view-space z axis") {
    const Mat4 view = lookAt(Vec3{0, 0, 5}, Vec3{0, 0, 0}, Vec3{0, 1, 0});
    const Vec3 target = transformPoint(view, Vec3{0, 0, 0});
    CHECK(target.x == doctest::Approx(0.0f).epsilon(0.001));
    CHECK(target.y == doctest::Approx(0.0f).epsilon(0.001));
    CHECK(target.z == doctest::Approx(-5.0f).epsilon(0.001));
}

TEST_CASE("quaternion rotates about its axis") {
    const Quat q = quatFromAxisAngle(Vec3{0, 1, 0}, kPi * 0.5f);
    const Vec3 rotated = rotate(q, Vec3{0, 0, 1});
    CHECK(nearlyEqual(rotated, Vec3{1, 0, 0}));
}

TEST_CASE("OrbitCamera framing sets distance from radius") {
    OrbitCamera cam;
    cam.frameSphere(Vec3{1, 2, 3}, 2.0f);
    CHECK(cam.target().x == doctest::Approx(1.0f));
    CHECK(cam.distance() > 2.0f);  // fits the sphere in view
    const Vec3 eye = cam.eye();
    CHECK(length(eye - cam.target()) == doctest::Approx(cam.distance()).epsilon(0.001));
}

TEST_CASE("OrbitCamera dolly scales distance and clamps to a minimum") {
    OrbitCamera cam;
    cam.setDistance(10.0f);
    cam.dolly(0.5f);
    CHECK(cam.distance() == doctest::Approx(5.0f));
    cam.dolly(0.0f);  // clamped, never zero
    CHECK(cam.distance() > 0.0f);
}

TEST_CASE("OrbitCamera pitch is clamped away from the poles") {
    OrbitCamera cam;
    cam.orbit(0.0f, 100.0f);  // huge upward orbit
    const Vec3 eye = cam.eye();
    // Never exactly on the pole (would make up/forward parallel).
    CHECK(std::abs(eye.y) < cam.distance());
}

namespace {

GestureSample feed(GestureRecognizer& rec, std::vector<TouchPoint> frame) {
    return rec.update(frame);
}

}  // namespace

TEST_CASE("first touch frame only establishes a baseline") {
    GestureRecognizer rec;
    const GestureSample s = feed(rec, {TouchPoint{1, {10, 10}, 5.0f, 1.0f}});
    CHECK(s.kind == GestureKind::None);
    CHECK(s.fingertipCount == 1);
}

TEST_CASE("one fingertip dragging is Orbit") {
    GestureRecognizer rec;
    feed(rec, {TouchPoint{1, {10, 10}, 5.0f, 1.0f}});
    const GestureSample s = feed(rec, {TouchPoint{1, {20, 15}, 5.0f, 1.0f}});
    CHECK(s.kind == GestureKind::Orbit);
    CHECK(s.delta.x == doctest::Approx(10.0f));
    CHECK(s.delta.y == doctest::Approx(5.0f));
}

TEST_CASE("two fingertips translating together is Pan") {
    GestureRecognizer rec;
    feed(rec, {TouchPoint{1, {0, 0}, 5, 1}, TouchPoint{2, {10, 0}, 5, 1}});
    const GestureSample s =
        feed(rec, {TouchPoint{1, {5, 5}, 5, 1}, TouchPoint{2, {15, 5}, 5, 1}});
    CHECK(s.kind == GestureKind::Pan);
    CHECK(s.delta.x == doctest::Approx(5.0f));
    CHECK(s.delta.y == doctest::Approx(5.0f));
}

TEST_CASE("two fingertips spreading is Dolly") {
    GestureRecognizer rec;
    feed(rec, {TouchPoint{1, {-10, 0}, 5, 1}, TouchPoint{2, {10, 0}, 5, 1}});
    const GestureSample s =
        feed(rec, {TouchPoint{1, {-20, 0}, 5, 1}, TouchPoint{2, {20, 0}, 5, 1}});
    CHECK(s.kind == GestureKind::Dolly);
    CHECK(s.dollyFactor == doctest::Approx(2.0f));
}

TEST_CASE("large-radius contacts are rejected as palm") {
    GestureConfig config;
    config.palmRadiusPx = 40.0f;
    GestureRecognizer rec(config);
    // One fingertip plus a palm; only the fingertip survives.
    feed(rec, {TouchPoint{1, {10, 10}, 5, 1}, TouchPoint{9, {200, 200}, 80, 1}});
    const GestureSample s =
        feed(rec, {TouchPoint{1, {20, 10}, 5, 1}, TouchPoint{9, {201, 201}, 80, 1}});
    CHECK(s.rejectedCount == 1);
    CHECK(s.fingertipCount == 1);
    CHECK(s.kind == GestureKind::Orbit);  // palm did not turn it into a 2-finger gesture
}

TEST_CASE("strict contact count rejects a frame where a finger is added") {
    GestureRecognizer rec;
    feed(rec, {TouchPoint{1, {0, 0}, 5, 1}});
    const GestureSample s =
        feed(rec, {TouchPoint{1, {5, 0}, 5, 1}, TouchPoint{2, {10, 0}, 5, 1}});
    CHECK(s.kind == GestureKind::None);  // count changed 1 -> 2, ambiguous
}
