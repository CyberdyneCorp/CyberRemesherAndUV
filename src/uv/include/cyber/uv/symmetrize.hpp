#pragma once

#include <cmath>
#include <cstddef>
#include <limits>
#include <span>
#include <vector>

#include "cyber/core/math.hpp"
#include "cyber/core/mesh.hpp"
#include "cyber/uv/common.hpp"

// Island symmetrization in UV space (uv-editing spec, "Gesture unwrap" —
// automatic symmetrization — and "2D layout tools" — partial symmetrization
// around a cursor). The mirror axis is a point plus a (not necessarily unit)
// direction.
namespace cyber::uv {

// Reflects a point across the line through `axisPoint` with direction
// `axisDir`.
[[nodiscard]] inline Vec2 mirrorPoint(Vec2 p, Vec2 axisPoint, Vec2 axisDir) {
    const float len2 = axisDir.x * axisDir.x + axisDir.y * axisDir.y;
    if (len2 <= 0.0f) {
        return p;
    }
    const Vec2 d = p - axisPoint;
    const float t = (d.x * axisDir.x + d.y * axisDir.y) / len2;
    const Vec2 proj = {axisPoint.x + axisDir.x * t, axisPoint.y + axisDir.y * t};
    return {2.0f * proj.x - p.x, 2.0f * proj.y - p.y};
}

// Mirrors every UV of an island across the axis (flips it in place).
inline void mirrorIslandUv(Mesh& mesh, std::span<const FaceId> island, Vec2 axisPoint,
                           Vec2 axisDir) {
    forEachIslandUv(mesh, island, [&](Vec2& uv) { uv = mirrorPoint(uv, axisPoint, axisDir); });
}

// Partial symmetrization: pulls each UV toward the midpoint of itself and its
// nearest mirrored counterpart within the island, by `strength` in [0,1]. At
// strength 1 the island becomes symmetric about the axis. O(k^2) in the corner
// count k, which is intended for the local (under-cursor) region.
inline void symmetrizeIslandUv(Mesh& mesh, std::span<const FaceId> island, Vec2 axisPoint,
                               Vec2 axisDir, float strength = 1.0f) {
    std::vector<Vec2>* uv = uvColumn(mesh);
    if (uv == nullptr) {
        return;
    }
    std::vector<std::size_t> loops;
    std::vector<Vec2> pts;
    for (const FaceId face : island) {
        for (const LoopId loop : mesh.faceLoops(face)) {
            loops.push_back(static_cast<std::size_t>(loop.value));
            pts.push_back((*uv)[static_cast<std::size_t>(loop.value)]);
        }
    }

    std::vector<Vec2> mirrored(pts.size());
    for (std::size_t i = 0; i < pts.size(); ++i) {
        mirrored[i] = mirrorPoint(pts[i], axisPoint, axisDir);
    }

    for (std::size_t i = 0; i < pts.size(); ++i) {
        std::size_t best = i;
        float bestDist = std::numeric_limits<float>::max();
        for (std::size_t j = 0; j < pts.size(); ++j) {
            const Vec2 diff = pts[i] - mirrored[j];
            const float d = diff.x * diff.x + diff.y * diff.y;
            if (d < bestDist) {
                bestDist = d;
                best = j;
            }
        }
        const Vec2 target = (pts[i] + mirrored[best]) * 0.5f;
        (*uv)[loops[i]] = lerp(pts[i], target, strength);
    }
}

}  // namespace cyber::uv
