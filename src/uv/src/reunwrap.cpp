#include "cyber/uv/reunwrap.hpp"

#include <algorithm>
#include <cmath>
#include <vector>

#include "cyber/uv/transforms.hpp"
#include "cyber/uv/unwrap.hpp"

namespace cyber::uv {
namespace {

// The island containing `face`, or an empty vector when `face` is in none.
[[nodiscard]] std::vector<FaceId> islandContaining(const Mesh& mesh, FaceId face,
                                                   const SeamSet& seams) {
    for (std::vector<FaceId>& island : computeIslands(mesh, seams)) {
        if (std::find(island.begin(), island.end(), face) != island.end()) {
            return std::move(island);
        }
    }
    return {};
}

// Fits `island`'s current UVs inside `target`, uniformly and centred.
//
// One scale factor for both axes: see the header for why filling the box exactly would be a
// shear that undoes the conformal solve.
void fitIslandInto(Mesh& mesh, std::span<const FaceId> island, const Bounds2& target) {
    const Bounds2 now = islandUvBounds(mesh, island);
    if (!now.valid() || !target.valid()) {
        return;
    }
    const Vec2 have = now.size();
    const Vec2 want = target.size();
    // A degenerate axis (a straight-line island) has nothing to scale along, so it is only
    // translated. Scaling by the other axis' factor would be arbitrary.
    float scale = 1.0f;
    if (have.x > 0.0f && have.y > 0.0f) {
        scale = std::fmin(want.x / have.x, want.y / have.y);
    } else if (have.x > 0.0f) {
        scale = want.x / have.x;
    } else if (have.y > 0.0f) {
        scale = want.y / have.y;
    }
    if (scale > 0.0f && scale != 1.0f) {
        scaleIslandUv(mesh, island, {scale, scale}, now.center());
    }
    // Re-read rather than predicting the post-scale box: the scale pivots on the pre-scale
    // centre, and deriving the new centre arithmetically would drift from what was written.
    const Bounds2 scaled = islandUvBounds(mesh, island);
    if (scaled.valid()) {
        translateIslandUv(mesh, island, target.center() - scaled.center());
    }
}

}  // namespace

ReunwrapResult reunwrapIsland(Mesh& mesh, FaceId face, const SeamSet& seams,
                              const UnwrapOptions& options) {
    ReunwrapResult out;
    if (!mesh.isAlive(face)) {
        return out;
    }
    // Refused rather than created: writing one island into a fresh column would leave every
    // other island's corners zero-initialized, which reads downstream as a real layout with
    // those islands collapsed at the origin.
    if (uvColumn(mesh) == nullptr) {
        out.needsWholeMeshUnwrap = true;
        return out;
    }

    const std::vector<FaceId> island = islandContaining(mesh, face, seams);
    if (island.empty()) {
        return out;
    }
    out.faces = island.size();

    // The footprint to land back in, captured BEFORE the solve overwrites the UVs it is
    // measured from.
    const Bounds2 previous = islandUvBounds(mesh, island);

    const UnwrapResult solved = lscmUnwrap(mesh, island, options);
    out.iterations = solved.iterations;
    if (!solved.ok) {
        // Nothing has been written yet — `writeIslandUv` is the only mutation and it has not
        // run — so a failed solve leaves every UV in the mesh exactly as it was.
        return out;
    }
    writeIslandUv(mesh, island, solved);
    fitIslandInto(mesh, island, previous);
    out.ok = true;
    return out;
}

}  // namespace cyber::uv
