#pragma once

#include <cmath>

#include "cyber/core/math.hpp"

namespace cyber::bake {

// The tangent basis of a UV-mapped triangle (Lengyel's method: position
// gradient along the UV gradient, orthonormalised against the normal). This is
// the SINGLE definition used both when baking and when exporting the mesh, so a
// baked tangent-space normal map renders without seams or inverted channels
// (surface-baking spec: "the tangent basis used for baking SHALL be identical
// to the one exported with the mesh"). Task 11.4's numeric consistency check
// exercises this function; the in-viewer glTF check + movable-light preview are
// viewport-side.
struct TangentFrame {
    Vec3 tangent;
    Vec3 bitangent;
    Vec3 normal;
};

[[nodiscard]] inline TangentFrame tangentFrame(Vec3 p0, Vec3 p1, Vec3 p2, Vec2 uv0, Vec2 uv1,
                                               Vec2 uv2, Vec3 n) {
    const Vec3 e1 = p1 - p0, e2 = p2 - p0;
    const Vec2 d1 = uv1 - uv0, d2 = uv2 - uv0;
    const float det = d1.x * d2.y - d2.x * d1.y;
    Vec3 t;
    if (std::fabs(det) < 1e-20f) {
        // Degenerate UVs: any vector perpendicular to the normal.
        t = std::fabs(n.x) < 0.9f ? cross(n, Vec3{1, 0, 0}) : cross(n, Vec3{0, 1, 0});
    } else {
        const float f = 1.0f / det;
        t = (e1 * d2.y - e2 * d1.y) * f;
    }
    TangentFrame frame;
    frame.normal = normalized(n);
    frame.tangent = normalized(t - frame.normal * dot(frame.normal, t));
    frame.bitangent = cross(frame.normal, frame.tangent);
    return frame;
}

// Movable preview light for the bake-result viewport preview (11.4). The
// position is what the Move action repositions; rendering is viewport-side.
struct PreviewLight {
    Vec3 position{2.0f, 2.0f, 2.0f};
    Vec3 color{1.0f, 1.0f, 1.0f};
    float intensity = 1.0f;
};

}  // namespace cyber::bake
