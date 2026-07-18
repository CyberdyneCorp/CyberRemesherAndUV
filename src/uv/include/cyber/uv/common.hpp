#pragma once

#include <cstddef>
#include <limits>
#include <string_view>
#include <vector>

#include "cyber/core/math.hpp"
#include "cyber/core/mesh.hpp"

// Shared helpers for the UV-editing module (uv-editing spec). UVs live in a
// per-corner (per-loop) Vec2 attribute named "uv" on the Mesh, matching the
// convention used by the baker (surface-baking spec): one UV value per loop,
// indexed by LoopId::value.
namespace cyber::uv {

inline constexpr std::string_view kUvAttributeName = "uv";

// Axis-aligned 2D bounds used for island bounding boxes and packing.
struct Bounds2 {
    Vec2 mn{std::numeric_limits<float>::max(), std::numeric_limits<float>::max()};
    Vec2 mx{std::numeric_limits<float>::lowest(), std::numeric_limits<float>::lowest()};

    void expand(Vec2 p) {
        mn = {std::fmin(mn.x, p.x), std::fmin(mn.y, p.y)};
        mx = {std::fmax(mx.x, p.x), std::fmax(mx.y, p.y)};
    }
    [[nodiscard]] bool valid() const { return mn.x <= mx.x && mn.y <= mx.y; }
    [[nodiscard]] Vec2 size() const { return {mx.x - mn.x, mx.y - mn.y}; }
    [[nodiscard]] Vec2 center() const { return {(mn.x + mx.x) * 0.5f, (mn.y + mx.y) * 0.5f}; }
    [[nodiscard]] float area() const {
        const Vec2 s = size();
        return s.x * s.y;
    }

    // Strict interior overlap: two boxes that merely share an edge/corner
    // (zero-area intersection) are NOT considered overlapping. `slack`
    // shrinks each box before testing (a positive value tolerates small
    // touching gaps).
    [[nodiscard]] static bool overlap(const Bounds2& a, const Bounds2& b, float slack = 0.0f) {
        return a.mx.x - slack > b.mn.x && b.mx.x - slack > a.mn.x && a.mx.y - slack > b.mn.y &&
               b.mx.y - slack > a.mn.y;
    }
};

// Read/write access to the per-loop "uv" attribute column. Returns nullptr
// when the mesh carries no UVs.
[[nodiscard]] inline std::vector<Vec2>* uvColumn(Mesh& mesh) {
    return mesh.cornerAttributes().find<Vec2>(std::string(kUvAttributeName));
}
[[nodiscard]] inline const std::vector<Vec2>* uvColumn(const Mesh& mesh) {
    return mesh.cornerAttributes().find<Vec2>(std::string(kUvAttributeName));
}
// Creates the column (sized to the corner count) if it does not exist yet.
[[nodiscard]] inline std::vector<Vec2>& ensureUvColumn(Mesh& mesh) {
    return mesh.cornerAttributes().create<Vec2>(std::string(kUvAttributeName));
}

// Bounding box of every corner UV in an island.
[[nodiscard]] inline Bounds2 islandUvBounds(const Mesh& mesh, std::span<const FaceId> island) {
    Bounds2 box;
    const std::vector<Vec2>* uv = uvColumn(mesh);
    if (uv == nullptr) {
        return box;
    }
    for (const FaceId face : island) {
        for (const LoopId loop : mesh.faceLoops(face)) {
            box.expand((*uv)[static_cast<std::size_t>(loop.value)]);
        }
    }
    return box;
}

// Centroid of every corner UV in an island (average over loops).
[[nodiscard]] inline Vec2 islandUvCentroid(const Mesh& mesh, std::span<const FaceId> island) {
    const std::vector<Vec2>* uv = uvColumn(mesh);
    if (uv == nullptr) {
        return {};
    }
    Vec2 sum{};
    std::size_t count = 0;
    for (const FaceId face : island) {
        for (const LoopId loop : mesh.faceLoops(face)) {
            sum = sum + (*uv)[static_cast<std::size_t>(loop.value)];
            ++count;
        }
    }
    if (count == 0) {
        return {};
    }
    return sum * (1.0f / static_cast<float>(count));
}

// Applies `fn(Vec2&)` to every corner UV of the island in place. Each loop is
// visited exactly once (loops are per-face, so shared vertices are not
// transformed twice).
template <typename Fn>
void forEachIslandUv(Mesh& mesh, std::span<const FaceId> island, const Fn& fn) {
    std::vector<Vec2>* uv = uvColumn(mesh);
    if (uv == nullptr) {
        return;
    }
    for (const FaceId face : island) {
        for (const LoopId loop : mesh.faceLoops(face)) {
            fn((*uv)[static_cast<std::size_t>(loop.value)]);
        }
    }
}

}  // namespace cyber::uv
