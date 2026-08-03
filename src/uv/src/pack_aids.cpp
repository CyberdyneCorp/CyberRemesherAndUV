#include "cyber/uv/pack_aids.hpp"

#include <cmath>

#include "cyber/uv/distortion.hpp"
#include "cyber/uv/symmetrize.hpp"
#include "cyber/uv/transforms.hpp"

namespace cyber::uv {

PackResult packIslandsIntoRegion(Mesh& mesh, std::span<const std::vector<FaceId>> islands,
                                 const Bounds2& region, const PackParams& params) {
    PackResult failed;
    if (uvColumn(mesh) == nullptr) {
        // Refused, not silently skipped. `packIslands` tolerates a missing column by returning
        // a successful-looking result it never wrote — fine as an internal detail, a lie at an
        // API boundary, because the caller would be told the layout was repacked when there is
        // no layout at all.
        return failed;
    }
    const Vec2 extent = region.valid() ? region.size() : Vec2{0.0f, 0.0f};
    if (!region.valid() || extent.x <= 0.0f || extent.y <= 0.0f) {
        // Refused rather than clamped: a zero-area region would pack every island onto a
        // single point, which is a destroyed layout dressed up as a successful pack.
        return failed;
    }

    // Pack into 0-1 first, then map onto the region. Composing the existing packer instead of
    // reimplementing placement means the region variant cannot disagree with the unit-square
    // one about where islands go.
    PackResult result = packIslands(mesh, islands, params);
    if (!result.ok) {
        return result;
    }

    // ONE uniform scale for both axes, so a non-square region does not shear the islands. The
    // requirement is that packing applies only a uniform scale and a translation per island;
    // fitting 0-1 to a non-square region per-axis would violate exactly that.
    const float scale = std::fmin(extent.x, extent.y);
    const Vec2 origin{region.mn.x + (extent.x - scale) * 0.5f,
                      region.mn.y + (extent.y - scale) * 0.5f};

    std::vector<Vec2>* uv = uvColumn(mesh);
    if (uv == nullptr) {
        return result;
    }
    for (const std::vector<FaceId>& island : islands) {
        for (const FaceId face : island) {
            for (const LoopId loop : mesh.faceLoops(face)) {
                Vec2& p = (*uv)[static_cast<std::size_t>(loop.value)];
                p = {origin.x + p.x * scale, origin.y + p.y * scale};
            }
        }
    }
    // Report the placed boxes in REGION space, so a caller that trusts `placed` is not handed
    // unit-square coordinates the mesh no longer uses.
    for (PackedIsland& placed : result.islands) {
        placed.offset = {origin.x + placed.offset.x * scale, origin.y + placed.offset.y * scale};
        placed.scale *= scale;
        placed.placed.mn = {origin.x + placed.placed.mn.x * scale,
                            origin.y + placed.placed.mn.y * scale};
        placed.placed.mx = {origin.x + placed.placed.mx.x * scale,
                            origin.y + placed.placed.mx.y * scale};
    }
    result.scale *= scale;
    return result;
}

std::vector<std::size_t> flippedIslands(const Mesh& mesh,
                                        std::span<const std::vector<FaceId>> islands) {
    std::vector<std::size_t> out;
    for (std::size_t i = 0; i < islands.size(); ++i) {
        // The engine's own definition of a flipped island (net signed UV area), reused rather
        // than recomputed: a second implementation would let this disagree with the atlas
        // report and the per-face heatmap about the same island.
        if (measureDistortion(mesh, islands[i]).flipped) {
            out.push_back(i);
        }
    }
    return out;
}

void flipIslandUv(Mesh& mesh, std::span<const FaceId> island) {
    const Bounds2 box = islandUvBounds(mesh, island);
    if (!box.valid()) {
        return;
    }
    // Mirrored about the island's own centre along v, so the flip does not also translate the
    // island out of whatever region it was packed into. Involutive by construction.
    mirrorIslandUv(mesh, island, box.center(), Vec2{1.0f, 0.0f});
}

}  // namespace cyber::uv
