#pragma once

#include <cstddef>
#include <cstdint>
#include <vector>

#include "cyber/core/mesh.hpp"
#include "cyber/core/pipeline.hpp"
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
    // Exactly the vertices this projected onto the plane. Handed to
    // mirrorAcross so "on the plane" is a KNOWN set rather than a distance
    // test — see the overload below for why that distinction matters.
    std::vector<VertexId> onPlane;
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

// Mirror using a KNOWN centerline instead of a distance test.
//
// The tolerance overload decides "on the plane" by measuring, which is fine
// when the caller has no better information and wrong when it does. After
// snapBorderToPlane the caller knows exactly which vertices were projected, and
// re-deriving that set by distance is not merely redundant — it is a different,
// larger set. remeshSymmetric was passing 0.35 * mean edge length (the weld
// tolerance it needs for isTopologicallySymmetric), so every INTERIOR vertex
// that happened to lie within a third of an edge of the midplane was flattened
// onto it and then treated as its own twin. Those vertices are surface, not
// centerline: welding them instead of mirroring them is what left the seam
// residue (cheburashka: 3 boundary and 2 non-manifold edges).
//
// Vertices in `onPlane` are shared by both halves; every other vertex gets a
// reflected twin regardless of how close to the plane it sits.
MirrorReport mirrorAcross(Mesh& mesh, const Plane& plane, const std::vector<VertexId>& onPlane);

// Whether `mesh` is symmetric across `plane` in CONNECTIVITY: every vertex has a
// mirror partner and every face's mirror image is also a face. This is the
// property the pipeline promises, and it is checkable on the result rather than
// assumed from the construction.
[[nodiscard]] bool isTopologicallySymmetric(const Mesh& mesh, const Plane& plane,
                                            float tolerance = 1e-4f);

// The half-model target for a whole-model request. One rule, so a caller that
// wants to announce the halving before the solve starts and the solve itself
// cannot disagree about it.
[[nodiscard]] int symmetricHalfTarget(int wholeTargetQuads);

// What a forced-symmetry run did, for the caller to report or assert on.
struct SymmetryRunReport {
    // False when `axis` was None: the run was an ordinary remesh and every
    // field below is meaningless rather than zero-valued.
    bool applied = false;
    // The whole-model target the caller asked for, and the half-model target
    // the solve actually ran at.
    int requestedQuads = 0;
    int halfQuads = 0;
    std::size_t borderSnapped = 0;
    float maxBorderDrift = 0.0f;
    std::size_t membranesRemoved = 0;
    std::size_t mirroredVertices = 0;
    std::size_t mirroredFaces = 0;
    // Checked on the RESULT rather than assumed from the construction, because
    // the construction is exactly what a regression would break.
    bool topologicallySymmetric = false;
};

// Remesh `input` under forced-axis symmetry: cut it at the midplane, solve one
// half, and mirror that half's CONNECTIVITY back across the plane.
//
// Every argument after `axis` is forwarded to remesh() unchanged, so this is a
// drop-in for it. With SymmetryAxis::None it IS remesh(), byte for byte, and
// `report->applied` is false.
//
// `params.targetQuadCount` names the WHOLE model, so the half is solved for
// half of it. Without that halving the request would silently mean "per half"
// and a symmetric run would come back at twice the size that was asked for.
//
// This lives here, rather than in the caller that first needed it, because the
// CLI and the C ABI must produce the same mesh for the same request
// (engine-bindings spec, "Parity SHALL hold"). A hundred lines of splitting,
// border-snapping and mirroring copied into a second caller is a divergence
// waiting to happen, not parity.
//
// The result's statistics are recounted after mirroring: they otherwise
// describe the half that was solved, which is half of what the caller gets
// back. Fails the same way remesh() does; additionally returns a
// RunStatus::Error result when the input cannot be split at its midplane.
[[nodiscard]] PipelineResult remeshSymmetric(
    const Mesh& input, const Parameters& rawParams, SymmetryAxis axis,
    SymmetryRunReport* report = nullptr, ProgressSink* progress = nullptr,
    const CancelToken* cancel = nullptr, const QuadrangulatorFactory& quadrangulator = {},
    const QuadrangulatorFactory& fallbackQuadrangulator = {}, const Guidance* guidance = nullptr);

}  // namespace cyber::remesh
