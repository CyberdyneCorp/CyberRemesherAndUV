#pragma once

#include <algorithm>
#include <cmath>
#include <cstddef>
#include <unordered_map>
#include <vector>

#include "cyber/core/math.hpp"
#include "cyber/core/mesh.hpp"
#include "cyber/retopo/snapping.hpp"

// Symmetry (manual-retopology spec, "Symmetry"): mirror across a plane, snap
// plane-adjacent vertices onto the plane, and an "apply" command that bakes the
// mirror into real geometry. Reuses the plane math in snapping.hpp so the
// image-snap plane and the symmetry plane behave identically. Header-only and
// inline.
namespace cyber::retopo {

struct Symmetry {
    Plane plane;                      // mirror plane
    float weldTolerance = 1e-4f;      // vertices this close to the plane weld onto it
    bool workingSidePositive = true;  // which half is authored (mirrored to the other)
};

// Signed distance oriented so positive means "on the working side".
[[nodiscard]] inline float workingSideDistance(const Symmetry& sym, Vec3 p) {
    const float d = signedDistance(sym.plane, p);
    return sym.workingSidePositive ? d : -d;
}

// Snaps every vertex within the weld tolerance of the plane exactly onto it.
// Returns the number of vertices snapped.
inline std::size_t snapNearPlane(Mesh& mesh, const Symmetry& sym) {
    std::size_t snapped = 0;
    for (Index i = 0; i < mesh.vertexCapacity(); ++i) {
        const VertexId v{i};
        if (!mesh.isAlive(v)) {
            continue;
        }
        const Vec3 p = mesh.position(v);
        if (std::fabs(signedDistance(sym.plane, p)) <= sym.weldTolerance) {
            mesh.setPosition(v, projectToPlane(sym.plane, p));
            ++snapped;
        }
    }
    return snapped;
}

// Bakes the mirror into real geometry: every face lying wholly on the working
// side (within tolerance) gets a mirrored twin with reversed winding, welding
// on-plane vertices to themselves so the seam stays manifold. Returns the
// number of faces added.
inline std::size_t applySymmetry(Mesh& mesh, const Symmetry& sym,
                                 const SurfaceSnapper* snap = nullptr) {
    snapNearPlane(mesh, sym);

    // Collect the working-side faces up-front (face ids are captured before we
    // start adding the mirrored geometry).
    std::vector<std::vector<VertexId>> toMirror;
    for (Index i = 0; i < mesh.faceCapacity(); ++i) {
        const FaceId f{i};
        if (!mesh.isAlive(f)) {
            continue;
        }
        const std::vector<VertexId> verts = mesh.faceVertices(f);
        const bool onWorkingSide = std::all_of(verts.begin(), verts.end(), [&](VertexId v) {
            return workingSideDistance(sym, mesh.position(v)) >= -sym.weldTolerance;
        });
        // A face lying entirely on the mirror plane welds all its vertices back
        // onto themselves, so mirroring it just re-adds the same face with
        // reversed winding — a coincident, degenerate duplicate. Require at
        // least one vertex strictly off the plane before mirroring.
        const bool anyOffPlane = std::any_of(verts.begin(), verts.end(), [&](VertexId v) {
            return std::fabs(signedDistance(sym.plane, mesh.position(v))) > sym.weldTolerance;
        });
        if (onWorkingSide && anyOffPlane) {
            toMirror.push_back(verts);
        }
    }

    std::unordered_map<Index, VertexId> twin;
    auto mirrorVertex = [&](VertexId v) {
        const Vec3 p = mesh.position(v);
        if (std::fabs(signedDistance(sym.plane, p)) <= sym.weldTolerance) {
            return v;  // on the plane: shared by both halves
        }
        const auto it = twin.find(v.value);
        if (it != twin.end()) {
            return it->second;
        }
        Vec3 m = mirrorAcrossPlane(sym.plane, p);
        if (snap != nullptr && !snap->empty()) {
            m = snap->snapToSurface(m).point;
        }
        const VertexId nv = mesh.addVertex(m);
        twin.emplace(v.value, nv);
        return nv;
    };

    std::size_t added = 0;
    for (const std::vector<VertexId>& verts : toMirror) {
        std::vector<VertexId> mirrored;
        mirrored.reserve(verts.size());
        for (auto it = verts.rbegin(); it != verts.rend(); ++it) {  // reversed winding
            mirrored.push_back(mirrorVertex(*it));
        }
        if (mesh.addFace(mirrored).valid()) {
            ++added;
        }
    }
    return added;
}

}  // namespace cyber::retopo
