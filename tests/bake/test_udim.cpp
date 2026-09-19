#include <doctest.h>

#include <algorithm>
#include <array>
#include <cmath>
#include <cstdint>
#include <string>
#include <vector>

#include "cyber/bake/bake.hpp"
#include "cyber/core/mesh.hpp"
#include "cyber/core/progress.hpp"

using cyber::CancelToken;
using cyber::FaceId;
using cyber::Index;
using cyber::LoopId;
using cyber::Mesh;
using cyber::ProgressSink;
using cyber::Vec2;
using cyber::Vec3;
namespace bake = cyber::bake;

// UDIM-aware baking (surface-baking spec, "UDIM-aware baking").
//
// The case that matters here is "Occlusion crosses a tile boundary". A per-tile
// loop that rebuilds the acceleration structure from the faces whose UVs lie in
// the tile being written produces ambient occlusion that is entirely plausible
// and entirely wrong -- an arm stops shadowing a torso the moment the two are
// packed into different tiles -- and looking at the output does not reveal it.
// Everything else in this file is a guard on an interaction with a feature that
// already shipped: padding (#90) must not bleed across a tile seam, an id map
// (#88) must take one colour across the set, and an encoding (#87) must decode
// with one box across it.
namespace {

Mesh emptyWithUv() {
    Mesh mesh;
    mesh.cornerAttributes().create<Vec2>("uv");
    return mesh;
}

// One face with explicit positions and explicit UVs, so a case can put a face's
// layout in whatever tile it wants without a helper deciding for it.
FaceId addFace(Mesh& mesh, const std::vector<Vec3>& positions, const std::vector<Vec2>& uvs) {
    REQUIRE(positions.size() == uvs.size());
    std::vector<cyber::VertexId> verts;
    verts.reserve(positions.size());
    for (const Vec3& p : positions) {
        verts.push_back(mesh.addVertex(p));
    }
    const FaceId face = mesh.addFace(verts);
    auto* uv = mesh.cornerAttributes().find<Vec2>("uv");
    REQUIRE(uv != nullptr);
    std::size_t corner = 0;
    for (const LoopId l : mesh.faceLoops(face)) {
        (*uv)[l.value] = uvs[corner++];
    }
    return face;
}

// An axis-aligned quad in the y = height plane, spanning [x0, x1] x [z0, z1],
// whose UVs cover [u0, u1] x [v0, v1] of the layout. Wound so the face normal is
// +Y: the AO case below needs "above" to mean "inside the shaded hemisphere",
// and a plate wound the other way would open its hemisphere downward and report
// a floor as unoccluded however much geometry sat on top of it.
void addPlate(Mesh& mesh, float height, float x0, float x1, float z0, float z1, float u0, float u1,
              float v0, float v1) {
    addFace(
        mesh,
        {Vec3{x0, height, z0}, Vec3{x0, height, z1}, Vec3{x1, height, z1}, Vec3{x1, height, z0}},
        {Vec2{u0, v0}, Vec2{u0, v1}, Vec2{u1, v1}, Vec2{u1, v0}});
}

std::vector<int> tileNumbers(const bake::UdimLayout& layout) {
    std::vector<int> numbers;
    numbers.reserve(layout.tiles.size());
    for (const bake::UdimTile& tile : layout.tiles) {
        numbers.push_back(tile.number);
    }
    return numbers;
}

bake::BakeParams smallParams(int size) {
    bake::BakeParams params;
    params.width = size;
    params.height = size;
    return params;
}

// The 8-bit triple a texel holds, which is how an id map is compared: its
// colours are exact keys, never measurements.
std::array<std::uint8_t, 3> texelBytes(const bake::Image& image, int x, int y) {
    std::array<std::uint8_t, 3> out{};
    for (int c = 0; c < 3; ++c) {
        out[static_cast<std::size_t>(c)] =
            static_cast<std::uint8_t>(std::lround(image.at(x, y, c) * 255.0f));
    }
    return out;
}

bool imageHoldsColor(const bake::Image& image, const std::array<std::uint8_t, 3>& color) {
    for (int y = 0; y < image.height; ++y) {
        for (int x = 0; x < image.width; ++x) {
            if (texelBytes(image, x, y) == color) {
                return true;
            }
        }
    }
    return false;
}

}  // namespace

// ---- detection -----------------------------------------------------------

TEST_CASE("occupied tiles are detected, ascending, without a bake") {
    Mesh mesh = emptyWithUv();
    addPlate(mesh, 0.0f, 0, 1, 0, 1, 0.1f, 0.9f, 0.1f, 0.9f);  // tile 1001
    addPlate(mesh, 0.0f, 2, 3, 0, 1, 1.1f, 1.9f, 0.1f, 0.9f);  // tile 1002
    addPlate(mesh, 0.0f, 4, 5, 0, 1, 0.1f, 0.9f, 1.1f, 1.9f);  // tile 1011

    const bake::UdimLayout layout = bake::udimTiles(mesh);
    CHECK(tileNumbers(layout) == std::vector<int>{1001, 1002, 1011});
    CHECK(layout.unaddressableFaces == 0);
}

TEST_CASE("occupancy is exact overlap, not a bounding box") {
    // A triangle whose UV bounding box reaches into tile 1012 while the
    // triangle itself stops short of it: its hypotenuse runs along u + v = 1.9
    // and tile 1012 begins at u + v = 2. A bounding-box test reports four
    // tiles, and every reported tile is an image allocated.
    Mesh mesh = emptyWithUv();
    addFace(mesh, {Vec3{0, 0, 0}, Vec3{1, 0, 0}, Vec3{0, 0, 1}},
            {Vec2{0.5f, 0.5f}, Vec2{1.4f, 0.5f}, Vec2{0.5f, 1.4f}});

    CHECK(tileNumbers(bake::udimTiles(mesh)) == std::vector<int>{1001, 1002, 1011});
}

TEST_CASE("a layout stopping exactly at a tile border does not occupy the next tile") {
    Mesh mesh = emptyWithUv();
    addPlate(mesh, 0.0f, 0, 1, 0, 1, 0.0f, 1.0f, 0.0f, 1.0f);

    CHECK(tileNumbers(bake::udimTiles(mesh)) == std::vector<int>{1001});
}

TEST_CASE("a packed unit-square layout is the single tile 1001") {
    // The uv-editing spec's "A packed layout is the single tile 1001": the
    // packer targets the 0-1 square and allocates no tiles of its own.
    Mesh mesh = emptyWithUv();
    addPlate(mesh, 0.0f, 0, 1, 0, 1, 0.05f, 0.45f, 0.05f, 0.95f);
    addPlate(mesh, 0.0f, 2, 3, 0, 1, 0.55f, 0.95f, 0.05f, 0.95f);

    CHECK(tileNumbers(bake::udimTiles(mesh)) == std::vector<int>{1001});
}

TEST_CASE("UV coordinates outside the addressable grid are counted, not dropped") {
    Mesh mesh = emptyWithUv();
    addPlate(mesh, 0.0f, 0, 1, 0, 1, 0.1f, 0.9f, 0.1f, 0.9f);    // tile 1001
    addPlate(mesh, 0.0f, 2, 3, 0, 1, 10.1f, 10.9f, 0.1f, 0.9f);  // u = 10: no number
    addPlate(mesh, 0.0f, 4, 5, 0, 1, 0.1f, 0.9f, -1.9f, -1.1f);  // v < 0: no number

    const bake::UdimLayout layout = bake::udimTiles(mesh);
    CHECK(tileNumbers(layout) == std::vector<int>{1001});
    CHECK(layout.unaddressableFaces == 2);
}

TEST_CASE("a mesh with no UV layout occupies no tile") {
    Mesh mesh;
    mesh.addVertex(Vec3{0, 0, 0});
    CHECK(bake::udimTiles(mesh).tiles.empty());
}

// ---- cost ----------------------------------------------------------------

TEST_CASE("a three-tile layout costs exactly three images") {
    Mesh mesh = emptyWithUv();
    addPlate(mesh, 0.0f, 0, 1, 0, 1, 0.1f, 0.9f, 0.1f, 0.9f);
    addPlate(mesh, 0.0f, 2, 3, 0, 1, 1.1f, 1.9f, 0.1f, 0.9f);
    addPlate(mesh, 0.0f, 4, 5, 0, 1, 0.1f, 0.9f, 1.1f, 1.9f);

    const bake::UdimBakeResult baked =
        bake::bakeUdim(mesh, mesh, bake::BakeMap::Normal, smallParams(16));
    REQUIRE(baked.refusal == bake::UdimRefusal::None);
    REQUIRE(baked.tiles.size() == 3);
    CHECK(baked.tiles[0].tile.number == 1001);
    CHECK(baked.tiles[1].tile.number == 1002);
    CHECK(baked.tiles[2].tile.number == 1011);
    for (const bake::UdimTileBake& tile : baked.tiles) {
        CHECK(tile.result.image.width == 16);
        CHECK(tile.result.texelsCovered > 0);
    }
}

TEST_CASE("a single-tile layout through the UDIM path equals the ordinary bake") {
    Mesh mesh = emptyWithUv();
    addPlate(mesh, 0.0f, 0, 1, 0, 1, 0.0f, 1.0f, 0.0f, 1.0f);
    const bake::BakeParams params = smallParams(24);

    const bake::BakeResult plain = bake::bake(mesh, mesh, bake::BakeMap::Normal, params);
    const bake::UdimBakeResult baked = bake::bakeUdim(mesh, mesh, bake::BakeMap::Normal, params);

    REQUIRE(baked.tiles.size() == 1);
    CHECK(baked.tiles[0].tile.number == 1001);
    CHECK(baked.tiles[0].result.image.pixels == plain.image.pixels);
}

// ---- the case the whole feature turns on ---------------------------------

namespace {

// A floor plate whose UVs fill tile 1001, optionally with an occluder floating
// above it whose UVs lie in tile 1002. The occluder is therefore invisible to
// the tile being written and visible to every ray cast for it.
Mesh floorWithOccluder(bool occluder) {
    Mesh mesh = emptyWithUv();
    addPlate(mesh, 0.0f, -2, 2, -2, 2, 0.0f, 1.0f, 0.0f, 1.0f);
    if (occluder) {
        addPlate(mesh, 1.0f, -1, 1, -1, 1, 1.0f, 2.0f, 0.0f, 1.0f);
    }
    return mesh;
}

bake::BakeParams aoParams(int size) {
    bake::BakeParams params = smallParams(size);
    params.aoSamples = 64;
    params.aoRadius = 20.0f;
    params.cageDistance = 0.05f;
    params.paddingRadius = 0;
    return params;
}

}  // namespace

TEST_CASE("occlusion from geometry in another tile is present in the result") {
    const bake::BakeParams params = aoParams(16);
    const Mesh open = floorWithOccluder(false);
    const Mesh shadowed = floorWithOccluder(true);

    const bake::UdimBakeResult openBake =
        bake::bakeUdim(open, open, bake::BakeMap::AmbientOcclusion, params);
    const bake::UdimBakeResult shadowedBake =
        bake::bakeUdim(shadowed, shadowed, bake::BakeMap::AmbientOcclusion, params);

    REQUIRE(openBake.tiles.size() == 1);
    REQUIRE(shadowedBake.tiles.size() == 2);
    REQUIRE(shadowedBake.tiles[0].tile.number == 1001);

    // The centre of the floor sits directly under the occluder, whose UVs are
    // in tile 1002. An acceleration structure rebuilt from tile 1001's faces
    // alone reports the same openness as the unoccluded floor.
    const float openCentre = openBake.tiles[0].result.image.at(8, 8, 0);
    const float shadowedCentre = shadowedBake.tiles[0].result.image.at(8, 8, 0);
    CHECK(openCentre > 0.9f);
    CHECK(shadowedCentre < 0.7f);
    CHECK(shadowedCentre < openCentre - 0.2f);
}

TEST_CASE("the tile written is unchanged by which tile its occluder is packed into") {
    // The ordinary bake already casts against the whole Target and clips the
    // layout to the unit square, so it IS the reference for tile 1001: if the
    // UDIM path narrowed what the rays can see, the two would part.
    const bake::BakeParams params = aoParams(16);
    const Mesh mesh = floorWithOccluder(true);

    const bake::BakeResult plain = bake::bake(mesh, mesh, bake::BakeMap::AmbientOcclusion, params);
    const bake::UdimBakeResult baked =
        bake::bakeUdim(mesh, mesh, bake::BakeMap::AmbientOcclusion, params);

    REQUIRE(baked.tiles.size() == 2);
    CHECK(baked.tiles[0].result.image.pixels == plain.image.pixels);
}

// ---- the two ceilings ----------------------------------------------------

namespace {

Mesh threeTiles() {
    Mesh mesh = emptyWithUv();
    addPlate(mesh, 0.0f, 0, 1, 0, 1, 0.1f, 0.9f, 0.1f, 0.9f);
    addPlate(mesh, 0.0f, 2, 3, 0, 1, 1.1f, 1.9f, 0.1f, 0.9f);
    addPlate(mesh, 0.0f, 4, 5, 0, 1, 2.1f, 2.9f, 0.1f, 0.9f);
    return mesh;
}

}  // namespace

TEST_CASE("a per-tile overflow names the per-tile ceiling") {
    const Mesh mesh = threeTiles();
    bake::BakeParams params = smallParams(64);
    params.maxPixels = 1000;  // below one tile's 4096

    const bake::UdimBakeResult baked = bake::bakeUdim(mesh, mesh, bake::BakeMap::Normal, params);
    CHECK(baked.refusal == bake::UdimRefusal::PerTileCeiling);
    CHECK(baked.refusalMessage.find("PER-TILE") != std::string::npos);
    CHECK(baked.tiles.empty());
    // The detection still ran, so the refusal knows what it was asked for.
    CHECK(baked.layout.tiles.size() == 3);
}

TEST_CASE("an aggregate overflow names the aggregate ceiling and the tile count") {
    const Mesh mesh = threeTiles();
    bake::BakeParams params = smallParams(64);
    params.maxPixels = 8192;  // one tile of 4096 fits; three do not

    const bake::UdimBakeResult baked = bake::bakeUdim(mesh, mesh, bake::BakeMap::Normal, params);
    CHECK(baked.refusal == bake::UdimRefusal::AggregateCeiling);
    CHECK(baked.refusalMessage.find("AGGREGATE") != std::string::npos);
    CHECK(baked.refusalMessage.find('3') != std::string::npos);
    CHECK(baked.tiles.empty());
}

TEST_CASE("a set that fits both ceilings bakes") {
    const Mesh mesh = threeTiles();
    bake::BakeParams params = smallParams(16);
    params.maxPixels = 3 * 16 * 16;

    const bake::UdimBakeResult baked = bake::bakeUdim(mesh, mesh, bake::BakeMap::Normal, params);
    CHECK(baked.refusal == bake::UdimRefusal::None);
    CHECK(baked.tiles.size() == 3);
}

// ---- interactions with what already shipped ------------------------------

namespace {

// A Target carrying two materials, far apart in object space, so that a bake
// over it exercises both the id table and an object-space box that no single
// tile's UV content spans.
Mesh idTargetWithMaterials() {
    const std::vector<Vec3> p = {
        {-1, 0, -1}, {1, 0, -1}, {1, 0, 1}, {-1, 0, 1},
        {5, 0, -1},  {7, 0, -1}, {7, 0, 1}, {5, 0, 1},
    };
    const std::vector<std::vector<Index>> f = {{0, 1, 2, 3}, {4, 5, 6, 7}};
    Mesh mesh2 = Mesh::fromIndexed(p, f);
    auto& ids = mesh2.faceAttributes().create<std::int32_t>("material_id");
    ids[0] = 7;
    ids[1] = 3;
    return mesh2;
}

Mesh twoTilesOverOneMaterial() {
    Mesh mesh = emptyWithUv();
    addPlate(mesh, 0.0f, -0.9f, 0.0f, -0.9f, 0.9f, 0.0f, 1.0f, 0.0f, 1.0f);
    addPlate(mesh, 0.0f, 0.0f, 0.9f, -0.9f, 0.9f, 1.0f, 2.0f, 0.0f, 1.0f);
    return mesh;
}

}  // namespace

TEST_CASE("an id map is the same colour, and reports the same table, in every tile") {
    const Mesh low = twoTilesOverOneMaterial();
    const Mesh high = idTargetWithMaterials();
    bake::BakeParams params = smallParams(16);
    params.cageDistance = 0.2f;
    params.paddingRadius = 0;

    const bake::UdimBakeResult baked = bake::bakeUdim(low, high, bake::BakeMap::MaterialId, params);
    REQUIRE(baked.tiles.size() == 2);

    const std::array<std::uint8_t, 3> seven = bake::idColor(7);
    CHECK(texelBytes(baked.tiles[0].result.image, 8, 8) == seven);
    CHECK(texelBytes(baked.tiles[1].result.image, 8, 8) == seven);

    // The whole Target's table, identically, in both tiles: a host's saved
    // selection must not break when it crosses a tile.
    const std::vector<bake::IdColorEntry>& first = baked.tiles[0].result.encoding.idColors;
    const std::vector<bake::IdColorEntry>& second = baked.tiles[1].result.encoding.idColors;
    REQUIRE(first.size() == 2);
    REQUIRE(second.size() == first.size());
    for (std::size_t i = 0; i < first.size(); ++i) {
        CHECK(first[i].id == second[i].id);
        CHECK(first[i].color == second[i].color);
    }
    CHECK(first[0].id == 3);
    CHECK(first[1].id == 7);
}

TEST_CASE("an object-position map reports one box across the set") {
    const Mesh low = twoTilesOverOneMaterial();
    const Mesh high = idTargetWithMaterials();
    bake::BakeParams params = smallParams(16);
    params.cageDistance = 0.2f;

    const bake::UdimBakeResult baked =
        bake::bakeUdim(low, high, bake::BakeMap::ObjectPosition, params);
    REQUIRE(baked.tiles.size() == 2);

    const bake::BakeEncoding& a = baked.tiles[0].result.encoding;
    const bake::BakeEncoding& b = baked.tiles[1].result.encoding;
    CHECK(a.basis == bake::EncodingBasis::ObjectBounds);
    CHECK(a.boundsMin.x == doctest::Approx(b.boundsMin.x));
    CHECK(a.boundsMax.x == doctest::Approx(b.boundsMax.x));
    CHECK(a.boundsMin.z == doctest::Approx(b.boundsMin.z));
    CHECK(a.boundsMax.z == doctest::Approx(b.boundsMax.z));
    // The whole mesh's box, not one tile's: the far material-3 quad reaches
    // x = 7 and is in neither tile's UV content.
    CHECK(a.boundsMax.x == doctest::Approx(7.0f));
}

TEST_CASE("a relative density map divides by the whole set's mean") {
    // Two islands of equal UV area over surfaces of area 1 and 4, in separate
    // tiles: their absolute densities differ by four, so a PER-TILE mean would
    // report both tiles as exactly 1 and hide it.
    Mesh mesh = emptyWithUv();
    addPlate(mesh, 0.0f, 0, 1, 0, 1, 0.0f, 1.0f, 0.0f, 1.0f);
    addPlate(mesh, 0.0f, 4, 6, 0, 2, 1.0f, 2.0f, 0.0f, 1.0f);

    bake::BakeParams params = smallParams(16);
    params.paddingRadius = 0;

    const bake::UdimBakeResult absolute =
        bake::bakeUdim(mesh, mesh, bake::BakeMap::UvDensity, params);
    REQUIRE(absolute.tiles.size() == 2);

    double sum = 0.0;
    std::size_t defined = 0;
    for (const bake::UdimTileBake& tile : absolute.tiles) {
        for (const float value : tile.result.image.pixels) {
            if (value > 0.0f) {
                sum += static_cast<double>(value);
                ++defined;
            }
        }
    }
    REQUIRE(defined > 0);
    const auto expectedMean = static_cast<float>(sum / static_cast<double>(defined));
    CHECK(absolute.tiles[0].result.encoding.densityMean == doctest::Approx(expectedMean));
    CHECK(absolute.tiles[1].result.encoding.densityMean == doctest::Approx(expectedMean));

    params.densityNormalization = bake::DensityNormalization::Relative;
    const bake::UdimBakeResult relative =
        bake::bakeUdim(mesh, mesh, bake::BakeMap::UvDensity, params);
    REQUIRE(relative.tiles.size() == 2);
    for (std::size_t i = 0; i < 2; ++i) {
        const float absoluteValue = absolute.tiles[i].result.image.at(8, 8, 0);
        REQUIRE(absoluteValue > 0.0f);
        CHECK(relative.tiles[i].result.image.at(8, 8, 0) ==
              doctest::Approx(absoluteValue / expectedMean).epsilon(1e-4));
        CHECK(relative.tiles[i].result.encoding.densityMean == doctest::Approx(expectedMean));
    }
    // A per-tile mean would make both of these exactly 1.
    CHECK(relative.tiles[0].result.image.at(8, 8, 0) > 1.1f);
    CHECK(relative.tiles[1].result.image.at(8, 8, 0) < 0.9f);
}

TEST_CASE("a tile's padded band never holds the neighbouring tile's value") {
    // Two islands of different materials, one ending short of the seam in tile
    // 1001 and one beginning past it in tile 1002. Each tile's band has room to
    // grow toward the seam, and must continue its OWN island.
    Mesh low = emptyWithUv();
    addPlate(low, 0.0f, -0.9f, -0.1f, -0.9f, 0.9f, 0.4f, 0.9f, 0.1f, 0.9f);
    addPlate(low, 0.0f, 5.1f, 6.9f, -0.9f, 0.9f, 1.05f, 1.5f, 0.1f, 0.9f);

    const Mesh high = idTargetWithMaterials();
    bake::BakeParams params = smallParams(32);
    params.cageDistance = 0.3f;
    params.paddingRadius = 4;

    const bake::UdimBakeResult baked = bake::bakeUdim(low, high, bake::BakeMap::MaterialId, params);
    REQUIRE(baked.tiles.size() == 2);

    const std::array<std::uint8_t, 3> seven = bake::idColor(7);
    const std::array<std::uint8_t, 3> three = bake::idColor(3);
    CHECK(imageHoldsColor(baked.tiles[0].result.image, seven));
    CHECK_FALSE(imageHoldsColor(baked.tiles[0].result.image, three));
    CHECK(imageHoldsColor(baked.tiles[1].result.image, three));
    CHECK_FALSE(imageHoldsColor(baked.tiles[1].result.image, seven));
    // Not vacuous: both tiles actually grew a band.
    CHECK(baked.tiles[0].result.padding.texelsFilled > 0);
    CHECK(baked.tiles[1].result.padding.texelsFilled > 0);
    CHECK(baked.tiles[0].result.padding.mode == bake::PaddingMode::Nearest);
}

// ---- refusals and cancellation -------------------------------------------

TEST_CASE("a layout occupying no addressable tile is refused by name") {
    Mesh mesh = emptyWithUv();
    addPlate(mesh, 0.0f, 0, 1, 0, 1, 20.1f, 20.9f, 0.1f, 0.9f);

    const bake::UdimBakeResult baked =
        bake::bakeUdim(mesh, mesh, bake::BakeMap::Normal, smallParams(16));
    CHECK(baked.refusal == bake::UdimRefusal::NoOccupiedTiles);
    CHECK(baked.tiles.empty());
    CHECK(baked.layout.unaddressableFaces == 1);
}

TEST_CASE("an out-of-range parameter refuses the whole set") {
    const Mesh mesh = threeTiles();
    bake::BakeParams params = smallParams(16);
    params.paddingRadius = -1;

    const bake::UdimBakeResult baked = bake::bakeUdim(mesh, mesh, bake::BakeMap::Normal, params);
    CHECK(baked.refusal == bake::UdimRefusal::Parameters);
    CHECK(baked.tiles.empty());
    CHECK(baked.layout.tiles.size() == 3);
}

TEST_CASE("cancelling a UDIM bake abandons the whole set") {
    const Mesh mesh = threeTiles();
    bake::BakeParams params = aoParams(24);

    const CancelToken cancel;
    ProgressSink sink([&cancel](float progress, std::string_view) {
        if (progress > 0.4f) {
            cancel.requestCancel();
        }
    });
    const bake::UdimBakeResult baked =
        bake::bakeUdim(mesh, mesh, bake::BakeMap::AmbientOcclusion, params, &sink, &cancel);

    CHECK(baked.cancelled);
    CHECK(baked.tiles.empty());
    CHECK(baked.refusal == bake::UdimRefusal::None);
}

TEST_CASE("the tile numbering is the standard one") {
    CHECK(bake::udimTileNumber(0, 0) == 1001);
    CHECK(bake::udimTileNumber(9, 0) == 1010);
    CHECK(bake::udimTileNumber(0, 1) == 1011);
    CHECK(bake::udimTileNumber(3, 2) == 1024);
}
