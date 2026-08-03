#pragma once

#include <cstddef>
#include <span>
#include <vector>

#include "cyber/core/math.hpp"
#include "cyber/core/mesh.hpp"

// Symmetry-aware island stacking (uv-workflow spec, "Symmetry-aware UVs").
//
// For a symmetric model, a left/right island pair can either occupy the SAME UV space — halving
// their texel cost, because one texture region serves both — or stay unique, when the two sides
// need to differ. This is the stacking half; keeping them unique is what the atlas already does.
namespace cyber::uv {

// Two islands that are 3D mirror images of one another across the symmetry plane.
struct MirrorIslandPair {
    // Indices into the island list handed in. `primary` keeps its UVs; `mirror` is the one
    // moved onto it.
    std::size_t primary = 0;
    std::size_t mirror = 0;
};

// Finds island pairs that mirror across the plane through `planePoint` with normal
// `planeNormal`.
//
// Matched by GEOMETRY, not by island order: reflecting an island's face centroids across the
// plane must land on the other island's. Island order out of `computeIslands` is an artifact of
// face-id iteration, so pairing by index would pair unrelated shells on any real model.
//
// `tolerance` is in world units and is compared against the scale of the match, so a model that
// is only approximately symmetric still pairs while a non-symmetric one yields nothing rather
// than a wrong pairing. Islands lying ON the plane (self-symmetric, e.g. a centre strip) are NOT
// paired with themselves — stacking one onto itself is a no-op that would report false progress.
//
// Each island appears in at most one pair, and `primary` is always the lower index so the result
// is deterministic.
[[nodiscard]] std::vector<MirrorIslandPair> findMirrorIslandPairs(
    const Mesh& mesh, std::span<const std::vector<FaceId>> islands, Vec3 planePoint,
    Vec3 planeNormal, float tolerance);

// Stacks each pair's `mirror` island onto its `primary`, so both occupy identical UV space.
//
// Corner correspondence is established by mirrored VERTEX POSITION rather than by face or corner
// order, for the same reason the pairing is geometric: `cloneIslandUv` copies by island-order
// and corner index, which for a mirror pair maps the wrong corners and produces a scrambled
// shell that still looks plausible in the 2D view.
//
// Returns the number of islands actually stacked. A pair whose corners cannot be corresponded
// one-to-one within `tolerance` is SKIPPED rather than partially written, so a failed match
// leaves that island's UVs exactly as they were.
std::size_t stackMirroredIslands(Mesh& mesh, std::span<const std::vector<FaceId>> islands,
                                 std::span<const MirrorIslandPair> pairs, Vec3 planePoint,
                                 Vec3 planeNormal, float tolerance);

}  // namespace cyber::uv
