#pragma once

#include "cyber/core/math.hpp"

// An oriented plane and the three operations everything does with one.
//
// This lives in core because two independent modules need it and neither should
// depend on the other: the interactive retopology tools use a plane as both the
// image-snapping target and the symmetry plane, and the automatic remesher uses
// one to split a mesh in half and mirror the result. Sharing the definition is
// what makes "the symmetry plane" mean the same thing in both.
namespace cyber {

struct Plane {
    Vec3 point{0.0f, 0.0f, 0.0f};
    Vec3 normal{0.0f, 1.0f, 0.0f};
};

// Signed distance from `p` to the plane (positive on the normal side).
[[nodiscard]] inline float signedDistance(const Plane& plane, Vec3 p) {
    return dot(p - plane.point, normalized(plane.normal));
}

// Orthogonal projection of `p` onto the plane.
[[nodiscard]] inline Vec3 projectToPlane(const Plane& plane, Vec3 p) {
    const Vec3 n = normalized(plane.normal);
    return p - n * dot(p - plane.point, n);
}

// Reflection of `p` across the plane.
[[nodiscard]] inline Vec3 mirrorAcrossPlane(const Plane& plane, Vec3 p) {
    const Vec3 n = normalized(plane.normal);
    return p - n * (2.0f * dot(p - plane.point, n));
}

}  // namespace cyber
