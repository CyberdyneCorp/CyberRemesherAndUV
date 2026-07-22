#pragma once

#include <cmath>
#include <vector>

#include "cyber/core/math.hpp"
#include "cyber/core/mesh.hpp"
#include "cyber/retopo/neighbors.hpp"
#include "cyber/retopo/pins.hpp"
#include "cyber/retopo/snapping.hpp"

// Relax action (manual-retopology spec, "Core actions coherent across stages"):
// tangential Laplacian smoothing that keeps vertices on the Target surface.
// Grid corners are auto-pinned so regular patterns survive, a brush radius
// masks the affected region (visible-radius mask), and explicit pins are
// honoured. Header-only and inline.
namespace cyber::retopo {

struct RelaxParams {
    float strength = 0.5f;       // per-iteration blend toward the tangent target
    int iterations = 1;          // Jacobi sweeps
    Vec3 brushCenter{};          // visible-radius mask centre
    float brushRadius = 0.0f;    // <= 0 relaxes the whole mesh (no mask)
    bool autoPinCorners = true;  // pin low-valence (grid-corner) vertices
};

// Smooth 0..1 brush falloff; 1 at the centre, 0 at the radius.
[[nodiscard]] inline float brushFalloff(float distance, float radius) {
    if (radius <= 0.0f) {
        return 1.0f;
    }
    if (distance >= radius) {
        return 0.0f;
    }
    const float t = 1.0f - distance / radius;
    return t * t * (3.0f - 2.0f * t);  // smoothstep
}

// A vertex counts as a grid corner (auto-pin candidate) when its valence is at
// most two, which preserves the corners of a regular quad grid under Relax.
[[nodiscard]] inline bool isGridCorner(const Mesh& mesh, VertexId v) {
    return oneRing(mesh, v).size() <= 2;
}

inline void relax(Mesh& mesh, const RelaxParams& params, const PinSet* pins = nullptr,
                  const SurfaceSnapper* snap = nullptr) {
    const float radiusSquared = params.brushRadius * params.brushRadius;
    for (int iter = 0; iter < params.iterations; ++iter) {
        std::vector<std::pair<VertexId, Vec3>> updates;
        for (Index i = 0; i < mesh.vertexCapacity(); ++i) {
            const VertexId v{i};
            if (!mesh.isAlive(v)) {
                continue;
            }
            if (pins != nullptr && pins->isPinned(v)) {
                continue;
            }
            if (params.autoPinCorners && isGridCorner(mesh, v)) {
                continue;
            }
            const Vec3 pos = mesh.position(v);
            if (params.brushRadius > 0.0f &&
                lengthSquared(pos - params.brushCenter) > radiusSquared) {
                continue;
            }
            const Vec3 centroid = neighborCentroid(mesh, v);
            Vec3 delta = centroid - pos;
            const Vec3 n = vertexNormal(mesh, v);
            delta = delta - n * dot(delta, n);  // keep the move tangential
            const float w = params.strength *
                            brushFalloff(length(pos - params.brushCenter), params.brushRadius);
            updates.emplace_back(v, pos + delta * w);
        }
        for (const auto& [v, target] : updates) {
            const Vec3 p =
                (snap != nullptr && !snap->empty()) ? snap->snapToSurface(target).point : target;
            mesh.setPosition(v, p);
        }
    }
}

}  // namespace cyber::retopo
