#pragma once

#include <cmath>

namespace cyber {

struct Vec2 {
    float x = 0.0f, y = 0.0f;

    friend Vec2 operator+(Vec2 a, Vec2 b) { return {a.x + b.x, a.y + b.y}; }
    friend Vec2 operator-(Vec2 a, Vec2 b) { return {a.x - b.x, a.y - b.y}; }
    friend Vec2 operator*(Vec2 a, float s) { return {a.x * s, a.y * s}; }
    friend bool operator==(Vec2 a, Vec2 b) { return a.x == b.x && a.y == b.y; }
};

struct Vec3 {
    float x = 0.0f, y = 0.0f, z = 0.0f;

    friend Vec3 operator+(Vec3 a, Vec3 b) { return {a.x + b.x, a.y + b.y, a.z + b.z}; }
    friend Vec3 operator-(Vec3 a, Vec3 b) { return {a.x - b.x, a.y - b.y, a.z - b.z}; }
    friend Vec3 operator*(Vec3 a, float s) { return {a.x * s, a.y * s, a.z * s}; }
    friend Vec3 operator/(Vec3 a, float s) { return {a.x / s, a.y / s, a.z / s}; }
    Vec3& operator+=(Vec3 o) {
        x += o.x;
        y += o.y;
        z += o.z;
        return *this;
    }
    friend bool operator==(Vec3 a, Vec3 b) { return a.x == b.x && a.y == b.y && a.z == b.z; }
};

struct Vec4 {
    float x = 0.0f, y = 0.0f, z = 0.0f, w = 0.0f;

    friend Vec4 operator+(Vec4 a, Vec4 b) { return {a.x + b.x, a.y + b.y, a.z + b.z, a.w + b.w}; }
    friend Vec4 operator-(Vec4 a, Vec4 b) { return {a.x - b.x, a.y - b.y, a.z - b.z, a.w - b.w}; }
    friend Vec4 operator*(Vec4 a, float s) { return {a.x * s, a.y * s, a.z * s, a.w * s}; }
    friend bool operator==(Vec4 a, Vec4 b) {
        return a.x == b.x && a.y == b.y && a.z == b.z && a.w == b.w;
    }
};

inline float dot(Vec3 a, Vec3 b) { return a.x * b.x + a.y * b.y + a.z * b.z; }
inline Vec3 cross(Vec3 a, Vec3 b) {
    return {a.y * b.z - a.z * b.y, a.z * b.x - a.x * b.z, a.x * b.y - a.y * b.x};
}
inline float lengthSquared(Vec3 v) { return dot(v, v); }
inline float length(Vec3 v) { return std::sqrt(lengthSquared(v)); }
inline Vec3 normalized(Vec3 v) {
    const float len = length(v);
    return len > 0.0f ? v / len : Vec3{};
}
inline Vec3 lerp(Vec3 a, Vec3 b, float t) { return a + (b - a) * t; }
inline Vec2 lerp(Vec2 a, Vec2 b, float t) { return a + (b - a) * t; }
inline Vec4 lerp(Vec4 a, Vec4 b, float t) { return a + (b - a) * (t); }
inline Vec3 min(Vec3 a, Vec3 b) {
    return {std::fmin(a.x, b.x), std::fmin(a.y, b.y), std::fmin(a.z, b.z)};
}
inline Vec3 max(Vec3 a, Vec3 b) {
    return {std::fmax(a.x, b.x), std::fmax(a.y, b.y), std::fmax(a.z, b.z)};
}

constexpr float kPi = 3.14159265358979323846f;
inline float degreesToRadians(float deg) { return deg * (kPi / 180.0f); }
inline float radiansToDegrees(float rad) { return rad * (180.0f / kPi); }

}  // namespace cyber
