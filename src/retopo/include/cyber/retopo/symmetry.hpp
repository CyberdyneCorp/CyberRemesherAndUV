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
    Plane plane;                     // mirror plane
    float weldTolerance = 1e-4f;     // vertices this close to the plane weld onto it
    bool workingSidePositive = true; // which half is authored (mirrored to the other)
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

// ---- re-symmetrize --------------------------------------------------------
//
// Restores symmetry to a mesh that has DRIFTED asymmetric (manual-retopology
// spec, "a re-symmetrize tool SHALL restore symmetry to a mesh that has
// drifted asymmetric", "preserving topology correspondence where it exists").
//
// Unlike applySymmetry this adds and removes NOTHING: topology is preserved
// exactly. Every vertex on the non-working side is matched to the working-side
// vertex whose mirror image is nearest, and — when that image is within
// `matchTolerance` — moved exactly onto it. Off-side vertices with no
// counterpart in range are left untouched and reported as `unmatched`, which
// is precisely the "where it exists" clause: geometry that only exists on one
// side survives re-symmetrize instead of being destroyed by it.

struct ResymmetrizeReport {
    // Vertices welded onto the plane first. Counts every vertex WITHIN the
    // weld tolerance, including ones already exactly on the plane — it is a
    // population count, not a change signal. `maxCorrection` is the change
    // signal, and it includes the weld displacement.
    std::size_t snapped = 0;
    std::size_t matched = 0;    // off-side vertices moved onto their mirror image
    std::size_t unmatched = 0;  // off-side vertices with no counterpart in range
    float maxCorrection = 0.0f; // largest displacement actually applied
};

namespace detail {

// Largest distance any vertex will travel when it welds onto the plane.
[[nodiscard]] inline float maxWeldDisplacement(const Mesh& mesh, const Symmetry& sym) {
    float worst = 0.0f;
    for (Index i = 0; i < mesh.vertexCapacity(); ++i) {
        const VertexId v{i};
        if (!mesh.isAlive(v)) {
            continue;
        }
        const float d = std::fabs(signedDistance(sym.plane, mesh.position(v)));
        if (d <= sym.weldTolerance) {
            worst = std::max(worst, d);
        }
    }
    return worst;
}

// Live vertices strictly on the working side, in ascending id order.
[[nodiscard]] inline std::vector<VertexId> workingSideVertices(const Mesh& mesh,
                                                               const Symmetry& sym) {
    std::vector<VertexId> result;
    for (Index i = 0; i < mesh.vertexCapacity(); ++i) {
        const VertexId v{i};
        if (mesh.isAlive(v) && workingSideDistance(sym, mesh.position(v)) > sym.weldTolerance) {
            result.push_back(v);
        }
    }
    return result;
}

// Mirror image of the working-side vertex nearest to `p`, or nothing when the
// nearest image is further away than `tolerance`. Ties break on the smaller
// vertex id so the result is deterministic.
[[nodiscard]] inline const Vec3* nearestMirrorImage(const std::vector<Vec3>& images, Vec3 p,
                                                    float tolerance) {
    const Vec3* best = nullptr;
    float bestDistance = 0.0f;
    for (const Vec3& image : images) {
        const float d = length(image - p);
        if (d > tolerance) {
            continue;
        }
        if (best == nullptr || d < bestDistance) {
            bestDistance = d;
            best = &image;
        }
    }
    return best;
}

}  // namespace detail

// Mirrors the working half onto the other half in place. `matchTolerance` is
// the world-space radius within which an off-side vertex is considered the
// counterpart of a working-side vertex's mirror image; callers scale it to the
// mesh (a fraction of the bounding-box diagonal) so the verdict is scale-free.
inline ResymmetrizeReport resymmetrize(Mesh& mesh, const Symmetry& sym, float matchTolerance) {
    ResymmetrizeReport report;
    report.maxCorrection = detail::maxWeldDisplacement(mesh, sym);
    report.snapped = snapNearPlane(mesh, sym);

    const std::vector<VertexId> sources = detail::workingSideVertices(mesh, sym);
    std::vector<Vec3> images;
    images.reserve(sources.size());
    for (const VertexId v : sources) {
        images.push_back(mirrorAcrossPlane(sym.plane, mesh.position(v)));
    }

    const float tolerance = matchTolerance > 0.0f ? matchTolerance : sym.weldTolerance;
    for (Index i = 0; i < mesh.vertexCapacity(); ++i) {
        const VertexId v{i};
        if (!mesh.isAlive(v)) {
            continue;
        }
        const Vec3 p = mesh.position(v);
        if (workingSideDistance(sym, p) >= -sym.weldTolerance) {
            continue;  // working side or on the plane: the authority, never moved
        }
        const Vec3* image = detail::nearestMirrorImage(images, p, tolerance);
        if (image == nullptr) {
            ++report.unmatched;
            continue;
        }
        report.maxCorrection = std::max(report.maxCorrection, length(*image - p));
        mesh.setPosition(v, *image);
        ++report.matched;
    }
    return report;
}

}  // namespace cyber::retopo
