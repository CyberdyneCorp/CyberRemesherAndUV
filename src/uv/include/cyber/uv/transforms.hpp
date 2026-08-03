#pragma once

#include <cmath>
#include <span>
#include <vector>

#include "cyber/core/math.hpp"
#include "cyber/core/mesh.hpp"
#include "cyber/uv/common.hpp"

// Island transforms in UV space (uv-editing spec, "On-model UV manipulation"
// and "2D layout tools"): translate, rotate and scale an island's UVs. These
// back the multitouch move/rotate/scale gestures in both the UV3D and UV2D
// views. Each acts on the per-loop "uv" attribute in place.
namespace cyber::uv {

inline void translateIslandUv(Mesh& mesh, std::span<const FaceId> island, Vec2 delta) {
    forEachIslandUv(mesh, island, [&](Vec2& uv) { uv = uv + delta; });
}

// Rotates the island's UVs by `radians` about `pivot` (counter-clockwise).
inline void rotateIslandUv(Mesh& mesh, std::span<const FaceId> island, float radians, Vec2 pivot) {
    const float c = std::cos(radians);
    const float s = std::sin(radians);
    forEachIslandUv(mesh, island, [&](Vec2& uv) {
        const Vec2 d = uv - pivot;
        uv = {pivot.x + d.x * c - d.y * s, pivot.y + d.x * s + d.y * c};
    });
}

// Non-uniform scale of the island's UVs about `pivot`.
inline void scaleIslandUv(Mesh& mesh, std::span<const FaceId> island, Vec2 factor, Vec2 pivot) {
    forEachIslandUv(mesh, island, [&](Vec2& uv) {
        uv = {pivot.x + (uv.x - pivot.x) * factor.x, pivot.y + (uv.y - pivot.y) * factor.y};
    });
}

// Uniform scale about the island centroid — the common gesture default.
inline void scaleIslandUv(Mesh& mesh, std::span<const FaceId> island, float factor) {
    scaleIslandUv(mesh, island, {factor, factor}, islandUvCentroid(mesh, island));
}

// Moves the UV VERTEX at `at` by `delta`, within one island (uv-workflow spec, "per-vertex mode").
//
// A "UV vertex" is a CLUSTER of coincident corners, not a mesh vertex — and that distinction is the
// whole design. UVs are per-corner, so several corners of an island usually share one UV position;
// moving only one of them would tear the island apart at a point the artist never cut. Moving every
// corner within `tolerance` of `at` keeps the island welded where it is welded.
//
// It also preserves SEAMS for free, without knowing anything about them: corners on opposite sides
// of a seam sit at DIFFERENT UVs by definition, so they fall outside each other's tolerance and move
// independently. That is exactly what a seam is.
//
// Returns the number of corners moved, so a caller can tell a hit from a miss — a drag that matched
// nothing must not be reported as an edit.
[[nodiscard]] inline std::size_t moveIslandUvVertex(Mesh& mesh, std::span<const FaceId> island,
                                                    Vec2 at, Vec2 delta, float tolerance) {
    std::vector<Vec2>* uv = uvColumn(mesh);
    if (uv == nullptr || tolerance < 0.0f) {
        return 0;
    }
    // Collected first, then written: `at` is compared against the ORIGINAL positions, so a corner
    // already moved cannot drag a neighbour along with it by drifting into range mid-loop.
    std::vector<std::size_t> matched;
    const float limit = tolerance * tolerance;
    for (const FaceId face : island) {
        for (const LoopId loop : mesh.faceLoops(face)) {
            const auto index = static_cast<std::size_t>(loop.value);
            const Vec2 d = (*uv)[index] - at;
            if (d.x * d.x + d.y * d.y <= limit) {
                matched.push_back(index);
            }
        }
    }
    for (const std::size_t index : matched) {
        (*uv)[index] = (*uv)[index] + delta;
    }
    return matched.size();
}

inline void rotateIslandUv(Mesh& mesh, std::span<const FaceId> island, float radians) {
    rotateIslandUv(mesh, island, radians, islandUvCentroid(mesh, island));
}

}  // namespace cyber::uv
