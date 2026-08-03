#pragma once

#include <span>

#include "cyber/core/mesh.hpp"
#include "cyber/uv/atlas.hpp"
#include "cyber/uv/common.hpp"
#include "cyber/uv/seams.hpp"

// Re-unwrap of a SINGLE island in place (uv-workflow spec, "An X gesture re-unwraps one
// island"): the localized counterpart to `unwrapAtlas`.
//
// The distinction that shapes this whole header: `unwrapAtlas` re-charts and REPACKS the
// entire mesh, so using it to service a gesture aimed at one region would re-lay-out the
// whole model. Re-unwrapping changes one island's internal parameterization and leaves its
// place in the atlas — and every other island's UVs — alone.
namespace cyber::uv {

struct ReunwrapResult {
    bool ok = false;
    // Faces in the island that was re-unwrapped.
    std::size_t faces = 0;
    // Set when the call was refused because the mesh carries no UVs at all, so the caller can
    // tell "nothing to re-unwrap yet" from a genuine solve failure and route to a whole-mesh
    // unwrap instead. See the refusal rationale on `reunwrapIsland`.
    bool needsWholeMeshUnwrap = false;
    // Iterations the conformal solve took, for the same diagnostic reasons as UnwrapResult.
    int iterations = 0;
};

// Re-unwraps the island containing `face`, keeping the island inside the UV footprint it
// already occupied.
//
// `seams` selects the island partition and MUST match whatever partition produced the UVs on
// screen, or the "island" re-unwrapped is not the one the artist pointed at. Callers that
// unwrapped with authored seams pass those; callers that used the automatic path pass the
// result of `autoSeams` with the same options, which is deterministic and therefore
// reproduces the original partition exactly.
//
// The island is fitted back into the footprint it already occupied: scaled by ONE factor to
// fit inside its previous bounding box and centred on that box's centre. The scale is uniform
// deliberately — stretching to exactly FILL the box would scale u and v by different factors
// whenever the new parameterization's aspect differs, and that shear destroys the conformality
// the solve just computed. An island whose aspect changed therefore occupies less than the
// full box, which is correct rather than a shortfall.
//
// REFUSES a mesh with no UV column (ok=false, needsWholeMeshUnwrap=true) instead of creating
// one. UVs are a single per-corner column, so creating it to write one island would
// zero-initialize every OTHER island's corners — a full, non-null UV stream in which every
// untouched island sits collapsed at the origin. That reads to every consumer as a real
// layout, so the caller must run a whole-mesh unwrap first; the policy of doing so belongs to
// the caller, and the refusal here makes the primitive impossible to misuse into it.
//
// Returns ok=false when `face` is dead or in no island, or when the conformal solve reports a
// degenerate island. A failure leaves every UV in the mesh untouched.
[[nodiscard]] ReunwrapResult reunwrapIsland(Mesh& mesh, FaceId face, const SeamSet& seams,
                                            const UnwrapOptions& options = {});

}  // namespace cyber::uv
