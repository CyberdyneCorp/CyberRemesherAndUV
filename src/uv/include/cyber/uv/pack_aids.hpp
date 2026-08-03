#pragma once

#include <span>
#include <vector>

#include "cyber/core/mesh.hpp"
#include "cyber/uv/common.hpp"
#include "cyber/uv/packing.hpp"

// Manual packing aids (uv-workflow spec, "Interactive island packing"): the hand-steerable
// half of packing, next to the automatic `packIslands`.
//
// An atlas layout is a starting point, not an answer — an artist repacks after re-unwrapping,
// after moving a seam, after deciding two shells should share space. These are the operations
// that make that steerable.
namespace cyber::uv {

// Packs `islands` into `region` rather than the whole unit square.
//
// `packIslands` normalizes into 0-1; this composes that with the affine map onto `region`, so
// the region variant cannot drift from the packer it wraps. Every island lands inside `region`.
//
// Preserves each island's internal parameterization: only a uniform scale and a translation
// are applied per island, so packing never changes how a shell is unwrapped. That is a
// requirement, not an implementation detail — a pack that reshaped islands would silently undo
// the artist's unwrapping choices.
//
// Returns the underlying pack result (whose `placed` boxes are in region space). Refused with
// ok=false for an invalid or zero-area region (rather than collapsing every island onto a
// point), and for a mesh with no UVs at all (rather than reporting a repack that never
// happened).
[[nodiscard]] PackResult packIslandsIntoRegion(Mesh& mesh,
                                               std::span<const std::vector<FaceId>> islands,
                                               const Bounds2& region,
                                               const PackParams& params = {});

// Islands whose net signed UV area is negative — mirrored shells, which bake inverted detail.
//
// Indices into `islands`, so the caller can point at the offending island rather than merely
// counting them. A mirrored island is a DEFECT rather than a magnitude, which is why this is a
// set membership question and not a score.
[[nodiscard]] std::vector<std::size_t> flippedIslands(
    const Mesh& mesh, std::span<const std::vector<FaceId>> islands);

// Mirrors one island's UVs in place about its own bounding-box centre, reversing its winding.
//
// About its own centre deliberately: any other axis would move the island as well as flip it,
// so a flip would double as an unrequested translation and could push a shell out of the
// region it was packed into. Flipping twice is the identity.
void flipIslandUv(Mesh& mesh, std::span<const FaceId> island);

}  // namespace cyber::uv
