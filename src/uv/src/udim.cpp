#include "cyber/uv/udim.hpp"

#include <algorithm>
#include <cmath>

#include "cyber/uv/transforms.hpp"

namespace cyber::uv {
namespace {

[[nodiscard]] int floorToTile(float value) { return static_cast<int>(std::floor(value)); }

}  // namespace

UdimCoord islandTile(const Mesh& mesh, std::span<const FaceId> island) {
    const Bounds2 box = islandUvBounds(mesh, island);
    if (!box.valid()) {
        return {};
    }
    // Tiles below zero are not representable in UDIM numbering, so they clamp to the first tile
    // rather than producing an index below 1001 that no exporter would accept.
    return {std::max(0, floorToTile(box.mn.x)), std::max(0, floorToTile(box.mn.y))};
}

bool islandStraddlesTiles(const Mesh& mesh, std::span<const FaceId> island) {
    const Bounds2 box = islandUvBounds(mesh, island);
    if (!box.valid()) {
        return false;
    }
    // The maximum is nudged inward before flooring: a shell packed flush to u = 1.0 ends exactly
    // on the border and belongs to the tile it fills, not the empty one it touches.
    constexpr float kEdge = 1e-5f;
    return floorToTile(box.mn.x) != floorToTile(box.mx.x - kEdge) ||
           floorToTile(box.mn.y) != floorToTile(box.mx.y - kEdge);
}

std::vector<int> occupiedUdimTiles(const Mesh& mesh,
                                  std::span<const std::vector<FaceId>> islands) {
    std::vector<int> tiles;
    tiles.reserve(islands.size());
    for (const std::vector<FaceId>& island : islands) {
        if (islandUvBounds(mesh, island).valid()) {
            tiles.push_back(udimIndex(islandTile(mesh, island)));
        }
    }
    std::sort(tiles.begin(), tiles.end());
    tiles.erase(std::unique(tiles.begin(), tiles.end()), tiles.end());
    return tiles;
}

void assignIslandToTile(Mesh& mesh, std::span<const FaceId> island, UdimCoord tile) {
    const UdimCoord current = islandTile(mesh, island);
    if (!islandUvBounds(mesh, island).valid()) {
        return;
    }
    // A WHOLE number of tiles, so the island's position inside its tile is preserved exactly:
    // moving a shell to another tile must not also re-arrange it.
    translateIslandUv(mesh, island,
                      Vec2{static_cast<float>(tile.u - current.u),
                           static_cast<float>(tile.v - current.v)});
}

}  // namespace cyber::uv
