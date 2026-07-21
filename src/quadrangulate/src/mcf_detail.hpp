#pragma once

// Internal shared helpers for the min-cost-flow integer layout (M2–M5), ported
// from QuadriFlow's field-math.hpp. Not part of the public API. Kept in double —
// QuadriFlow computes in double and the lattice floor-index is precision-sensitive.
#include <cmath>
#include <cstddef>
#include <limits>
#include <utility>

#include "cyber/core/math.hpp"
#include "cyber/quadrangulate/mcf_layout.hpp"

namespace cyber::remesh::mcf {

// Unsigned index from a signed loop counter (the tree builds -Werror=sign-conversion).
constexpr std::size_t uz(int v) { return static_cast<std::size_t>(v); }

inline int modulo(int a, int b) {
    const int r = a % b;
    return r < 0 ? r + b : r;
}

// Minimal double 3-vector so the geometry matches QuadriFlow's precision.
struct D3 {
    double x = 0, y = 0, z = 0;
};
inline D3 d3(const Vec3& v) { return {v.x, v.y, v.z}; }
inline Vec3 toVec3(D3 v) {
    return {static_cast<float>(v.x), static_cast<float>(v.y), static_cast<float>(v.z)};
}
inline D3 operator+(D3 a, D3 b) { return {a.x + b.x, a.y + b.y, a.z + b.z}; }
inline D3 operator-(D3 a, D3 b) { return {a.x - b.x, a.y - b.y, a.z - b.z}; }
inline D3 operator*(D3 a, double s) { return {a.x * s, a.y * s, a.z * s}; }
inline double dot(D3 a, D3 b) { return a.x * b.x + a.y * b.y + a.z * b.z; }
inline D3 cross(D3 a, D3 b) {
    return {a.y * b.z - a.z * b.y, a.z * b.x - a.x * b.z, a.x * b.y - a.y * b.x};
}
inline D3 normalize(D3 a) {
    const double n = std::sqrt(dot(a, a));
    return n > 1e-20 ? a * (1.0 / n) : a;
}

// field-math.hpp rshift90: rotate an integer lattice offset by amount*90 degrees.
inline Vec2i rshift90(Vec2i s, int amount) {
    if (amount & 1) {
        s = {-s.y, s.x};
    }
    if (amount >= 2) {
        s = {-s.x, -s.y};
    }
    return s;
}

// field-math.hpp rotate90_by: rotate q by amount*90 degrees about n.
inline D3 rotate90by(D3 q, D3 n, int amount) {
    return ((amount & 1) ? cross(n, q) : q) * (amount < 2 ? 1.0 : -1.0);
}

// field-math.hpp compat_orientation_extrinsic_index_4: the relative 4-RoSy index
// (a,b) aligning q0 and q1; b in [0,4). Returns (best_a, best_b).
inline std::pair<int, int> orientIndex4(D3 q0, D3 n0, D3 q1, D3 n1) {
    const D3 A[2] = {q0, cross(n0, q0)};
    const D3 B[2] = {q1, cross(n1, q1)};
    double best = -std::numeric_limits<double>::infinity();
    int ba = 0, bb = 0;
    for (int i = 0; i < 2; ++i) {
        for (int j = 0; j < 2; ++j) {
            const double s = std::abs(dot(A[i], B[j]));
            if (s > best) {
                best = s;
                ba = i;
                bb = j;
            }
        }
    }
    if (dot(A[ba], B[bb]) < 0) {
        bb += 2;
    }
    return {ba, bb};
}

}  // namespace cyber::remesh::mcf
