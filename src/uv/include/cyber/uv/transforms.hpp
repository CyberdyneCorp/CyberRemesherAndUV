#pragma once

#include <cmath>
#include <span>

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

inline void rotateIslandUv(Mesh& mesh, std::span<const FaceId> island, float radians) {
    rotateIslandUv(mesh, island, radians, islandUvCentroid(mesh, island));
}

}  // namespace cyber::uv
