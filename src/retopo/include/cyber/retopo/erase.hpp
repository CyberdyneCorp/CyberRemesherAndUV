#pragma once

#include <cstddef>
#include <vector>

#include "cyber/core/math.hpp"
#include "cyber/core/mesh.hpp"

// Erase action (manual-retopology spec, "Erase pressure scales coarseness"):
// removes geometry under the stroke, with the removal footprint growing with
// stylus pressure. Header-only and inline.
namespace cyber::retopo {

// Effective radius grows with pressure in [0,1]: half the base radius at zero
// pressure up to the full base radius (and beyond) at full pressure.
[[nodiscard]] inline float eraseRadius(float baseRadius, float pressure) {
    const float p = pressure < 0.0f ? 0.0f : pressure;
    return baseRadius * (0.5f + p);
}

// Removes every face whose centroid lies within the pressure-scaled radius of
// `center`, then any vertices left isolated. Returns the number of faces
// removed.
inline std::size_t erase(Mesh& mesh, Vec3 center, float baseRadius, float pressure) {
    const float radius = eraseRadius(baseRadius, pressure);
    const float radiusSquared = radius * radius;

    std::vector<FaceId> doomed;
    for (Index i = 0; i < mesh.faceCapacity(); ++i) {
        const FaceId f{i};
        if (!mesh.isAlive(f)) {
            continue;
        }
        if (lengthSquared(mesh.faceCentroid(f) - center) <= radiusSquared) {
            doomed.push_back(f);
        }
    }

    std::vector<VertexId> touched;
    for (const FaceId f : doomed) {
        for (const VertexId v : mesh.faceVertices(f)) {
            touched.push_back(v);
        }
        mesh.removeFace(f);
    }
    for (const VertexId v : touched) {
        if (mesh.isAlive(v) && mesh.vertexEdges(v).empty()) {
            mesh.removeIsolatedVertex(v);
        }
    }
    return doomed.size();
}

}  // namespace cyber::retopo
