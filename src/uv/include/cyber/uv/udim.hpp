#pragma once

#include <cstdint>
#include <span>
#include <vector>

#include "cyber/core/mesh.hpp"
#include "cyber/uv/common.hpp"

// UDIM tiles (uv-workflow spec, "UDIMs and multiple UV sets").
//
// The design point worth stating: a UDIM tile assignment is NOT stored anywhere. A tile IS a
// region of UV space — tile 1002 is simply u in [1, 2) — so an island's tile is derived from
// its UVs by construction. Persisting an assignment beside the UVs would create a second
// source of truth that could disagree with the geometry, and the disagreement would only
// surface at export time.
//
// Tile numbering is the industry convention: 1001 + u + 10 * v, so u runs 0-9 within a row.
namespace cyber::uv {

// Tile column/row of a UDIM index. Inverse of `udimIndex`.
struct UdimCoord {
    int u = 0;
    int v = 0;
};

[[nodiscard]] inline int udimIndex(UdimCoord tile) { return 1001 + tile.u + 10 * tile.v; }

[[nodiscard]] inline UdimCoord udimCoord(int index) {
    const int offset = index - 1001;
    return {offset % 10, offset / 10};
}

// The tile an island occupies, from the floor of its UV bounds' minimum corner.
//
// Uses the MINIMUM rather than the centroid so an island is attributed to the tile it starts
// in; an island straddling a tile border is a defect the caller can detect with
// `islandStraddlesTiles` rather than something to silently round away.
[[nodiscard]] UdimCoord islandTile(const Mesh& mesh, std::span<const FaceId> island);

// True when the island's UVs span more than one tile — it will be split across texture files
// on export, which is almost never intended.
[[nodiscard]] bool islandStraddlesTiles(const Mesh& mesh, std::span<const FaceId> island);

// Every occupied tile, ascending by UDIM index and deduplicated. This is the list an exporter
// needs to name its texture files.
[[nodiscard]] std::vector<int> occupiedUdimTiles(const Mesh& mesh,
                                                std::span<const std::vector<FaceId>> islands);

// Moves an island into `tile`, preserving its position WITHIN the tile.
//
// Translation only, and by a whole number of tiles, so the island's parameterization and its
// place inside its tile are both untouched — moving a shell to another tile must not also
// re-arrange it.
void assignIslandToTile(Mesh& mesh, std::span<const FaceId> island, UdimCoord tile);

}  // namespace cyber::uv
