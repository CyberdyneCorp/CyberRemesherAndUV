#pragma once

#include <cstddef>
#include <cstdint>

#include "cyber/core/mesh.hpp"
#include "cyber/core/plane.hpp"

// Exact topology symmetry (openspec/changes/add-zremesher-retopology, Phase F).
//
// The requirement for retopology is `left topology == mirrored right topology`,
// not `left shape ~= right shape`. Those are very different asks. Solving the
// whole mesh and hoping the numerics come back symmetric gives the second at
// best: a field solve on a symmetric surface is not a symmetric function of it,
// cones land where iteration order and floating-point ties put them, and the two
// halves end up with different edge counts that no amount of position averaging
// can reconcile.
//
// So symmetry is obtained by CONSTRUCTION instead: cut the input at the plane,
// solve one half, and mirror the connectivity. The mirrored half is then exact
// by definition rather than by luck, and the only thing that has to be handled
// carefully is the centerline where the two meet.
namespace cyber::remesh {

enum class SymmetryAxis : std::uint8_t {
    None,
    X,
    Y,
    Z,
};

// The plane for an axis through the mesh's bounding-box centre. Returns a plane
// with a zero normal for `None`, which every operation below treats as "no
// symmetry".
[[nodiscard]] Plane symmetryPlane(const Mesh& mesh, SymmetryAxis axis);

struct SymmetrySplit {
    // The working half, with every face that crossed the plane cut so the half's
    // border lies exactly ON it.
    Mesh half;
    // Vertices of `half` that sit on the plane. These are the ones the mirror
    // must weld rather than duplicate.
    std::vector<VertexId> centerline;
    bool valid = false;
};

// Cut `mesh` at `plane` and keep the side the normal points away from when
// `positiveSide` is false (and toward it when true).
//
// Faces crossing the plane are split at their edge crossings, so the border is
// exact rather than ragged — a ragged border snapped onto the plane afterwards
// would move vertices by up to an edge length and show as a seam.
[[nodiscard]] SymmetrySplit splitAtPlane(const Mesh& mesh, const Plane& plane, bool positiveSide,
                                         float tolerance = 1e-5f);

// Project every BORDER vertex of `mesh` onto `plane`.
//
// After a half is remeshed, its border does not sit on the plane any more: the
// isoline extraction ends where the isolines end, not where the cut was, so the
// border comes back inset and jagged by a fraction of an edge. Snapping by
// DISTANCE cannot fix that — the drift is larger than any tolerance that would
// still be safe.
//
// But when the input was CLOSED, the half has exactly one border and it is the
// cut, so every border vertex belongs on the plane by construction and can be
// projected regardless of how far it drifted. That is the caller's assertion to
// make, which is why this is a separate, explicit step rather than something
// mirrorAcross does on its own: on an input that already had holes, the other
// borders must not be dragged onto the plane.
//
// Returns the number projected, and the largest distance one had to move —
// reported rather than absorbed, because a large move means the remesh wandered
// and the result deserves inspection.
struct BorderSnapReport {
    std::size_t snapped = 0;
    float maxDistance = 0.0f;
    // Faces the snap pulled ENTIRELY into the plane. Those are membranes, not
    // surface — in the mirrored result they would sit inside the model as an
    // internal wall — so they are removed.
    std::size_t membranesRemoved = 0;
};
BorderSnapReport snapBorderToPlane(Mesh& mesh, const Plane& plane);

struct MirrorReport {
    std::size_t mirroredVertices = 0;
    std::size_t mirroredFaces = 0;
    std::size_t weldedCenterline = 0;
    bool valid = false;
};

// Mirror `mesh` across `plane` in place: every vertex off the plane gets a
// reflected twin, every face gets a reflected twin with reversed winding, and
// vertices ON the plane are shared by both sides rather than duplicated.
//
// The result is exactly symmetric in CONNECTIVITY: reflecting it across the
// plane reproduces it, vertex for vertex and face for face. Vertices within
// `tolerance` of the plane are snapped exactly onto it first, so "on the plane"
// is a decidable property rather than a floating-point coincidence.
MirrorReport mirrorAcross(Mesh& mesh, const Plane& plane, float tolerance = 1e-5f);

// Whether `mesh` is symmetric across `plane` in CONNECTIVITY: every vertex has a
// mirror partner and every face's mirror image is also a face. This is the
// property the pipeline promises, and it is checkable on the result rather than
// assumed from the construction.
[[nodiscard]] bool isTopologicallySymmetric(const Mesh& mesh, const Plane& plane,
                                            float tolerance = 1e-4f);

}  // namespace cyber::remesh
