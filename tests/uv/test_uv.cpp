#include <doctest.h>

#include <cmath>
#include <vector>

#include "cyber/core/math.hpp"
#include "cyber/core/mesh.hpp"
#include "cyber/uv/atlas.hpp"
#include "cyber/uv/common.hpp"
#include "cyber/uv/distortion.hpp"
#include "cyber/uv/packing.hpp"
#include "cyber/uv/seams.hpp"
#include "cyber/uv/transforms.hpp"
#include "cyber/uv/unwrap.hpp"

using cyber::FaceId;
using cyber::Index;
using cyber::Mesh;
using cyber::Vec2;
using cyber::Vec3;
using cyber::VertexId;
namespace uv = cyber::uv;

namespace {

// A quad in the z=0 plane spanning [0,w] x [0,h], tilted so LSCM must recover
// the planar shape from 3D rather than trivially copying coordinates.
Mesh makeQuad(float w, float h) {
    const std::vector<Vec3> p = {{0, 0, 0}, {w, 0, 0}, {w, h, 0}, {0, h, 0}};
    const std::vector<std::vector<Index>> f = {{0, 1, 2, 3}};
    return Mesh::fromIndexed(p, f);
}

// Unit cube, six quad faces — every face carries a distinct axis-aligned
// normal, so normal-coherent chart growth must isolate each into its own chart.
Mesh makeCube() {
    const std::vector<Vec3> p = {{0, 0, 0}, {1, 0, 0}, {1, 1, 0}, {0, 1, 0},
                                 {0, 0, 1}, {1, 0, 1}, {1, 1, 1}, {0, 1, 1}};
    const std::vector<std::vector<Index>> f = {{0, 3, 2, 1}, {4, 5, 6, 7}, {0, 1, 5, 4},
                                               {2, 3, 7, 6}, {1, 2, 6, 5}, {3, 0, 4, 7}};
    return Mesh::fromIndexed(p, f);
}

std::vector<FaceId> aliveFaces(const Mesh& mesh) {
    std::vector<FaceId> faces;
    for (Index i = 0; i < mesh.faceCapacity(); ++i) {
        if (mesh.isAlive(FaceId{i})) {
            faces.push_back(FaceId{i});
        }
    }
    return faces;
}

}  // namespace

TEST_CASE("LSCM unwrap of a flat quad has near-zero distortion") {
    Mesh mesh = makeQuad(2.0f, 1.0f);
    const std::vector<FaceId> island = aliveFaces(mesh);

    const uv::UnwrapResult result = uv::lscmUnwrap(mesh, island);
    REQUIRE(result.ok);
    REQUIRE(result.uv.size() == 4);

    uv::writeIslandUv(mesh, island, result);
    const uv::IslandDistortion dist = uv::measureDistortion(mesh, island);
    // A planar island maps conformally (a similarity), so angular distortion
    // must be essentially zero and no face flips.
    REQUIRE(dist.maxAngle < 1e-3f);
    REQUIRE_FALSE(dist.flipped);

    // A conformal map is a similarity, so it preserves edge-length ratios:
    // the quad's long side (2) over its short side (1) must stay 2:1 in UV,
    // regardless of the layout's orientation. Local vertices 0..3 correspond
    // to the input corners (0,0),(2,0),(2,1),(0,1).
    const auto uvLen = [](Vec2 a, Vec2 b) {
        const Vec2 d = a - b;
        return std::sqrt(d.x * d.x + d.y * d.y);
    };
    const float longSide = uvLen(result.uv[0], result.uv[1]);
    const float shortSide = uvLen(result.uv[1], result.uv[2]);
    REQUIRE(longSide / shortSide == doctest::Approx(2.0f).epsilon(0.02));
}

TEST_CASE("packing unit islands yields non-overlapping boxes in the unit square") {
    const int n = 7;
    std::vector<uv::Bounds2> boxes;
    for (int i = 0; i < n; ++i) {
        uv::Bounds2 b;
        b.expand({0.0f, 0.0f});
        b.expand({1.0f, 1.0f});  // unit island
        boxes.push_back(b);
    }

    const uv::PackResult packed = uv::packBoxes(boxes);
    REQUIRE(packed.ok);
    REQUIRE(packed.islands.size() == static_cast<std::size_t>(n));

    for (const uv::PackedIsland& island : packed.islands) {
        REQUIRE(island.placed.mn.x >= -1e-5f);
        REQUIRE(island.placed.mn.y >= -1e-5f);
        REQUIRE(island.placed.mx.x <= 1.0f + 1e-5f);
        REQUIRE(island.placed.mx.y <= 1.0f + 1e-5f);
    }
    for (std::size_t i = 0; i < packed.islands.size(); ++i) {
        for (std::size_t j = i + 1; j < packed.islands.size(); ++j) {
            REQUIRE_FALSE(uv::Bounds2::overlap(packed.islands[i].placed, packed.islands[j].placed));
        }
    }
}

TEST_CASE("packing preserves relative scale of unequal islands") {
    uv::Bounds2 small;
    small.expand({0, 0});
    small.expand({1, 1});
    uv::Bounds2 big;
    big.expand({0, 0});
    big.expand({2, 2});
    const std::vector<uv::Bounds2> boxes = {small, big};

    const uv::PackResult packed = uv::packBoxes(boxes);
    REQUIRE(packed.ok);
    const Vec2 sSmall = packed.islands[0].placed.size();
    const Vec2 sBig = packed.islands[1].placed.size();
    // The 2x island must remain twice the size after a uniform pack.
    REQUIRE(sBig.x == doctest::Approx(2.0f * sSmall.x).epsilon(1e-4));
    REQUIRE(packed.islands[0].scale == doctest::Approx(packed.islands[1].scale));
}

TEST_CASE("seam ring cuts a mesh into two islands") {
    // Two quads sharing an edge form one island; seaming the shared edge
    // splits them.
    const std::vector<Vec3> p = {{0, 0, 0}, {1, 0, 0}, {1, 1, 0},
                                 {0, 1, 0}, {2, 0, 0}, {2, 1, 0}};
    const std::vector<std::vector<Index>> f = {{0, 1, 2, 3}, {1, 4, 5, 2}};
    Mesh mesh = Mesh::fromIndexed(p, f);

    uv::SeamSet seams;
    REQUIRE(uv::computeIslands(mesh, seams).size() == 1);

    const cyber::EdgeId shared = mesh.edgeBetween(VertexId{1}, VertexId{2});
    REQUIRE(shared.valid());
    seams.mark(shared);
    REQUIRE(uv::computeIslands(mesh, seams).size() == 2);

    // Drawing over the seam sews it back.
    seams.toggle(shared);
    REQUIRE(uv::computeIslands(mesh, seams).size() == 1);
}

TEST_CASE("island UV translate offsets every corner") {
    Mesh mesh = makeQuad(1.0f, 1.0f);
    const std::vector<FaceId> island = aliveFaces(mesh);
    REQUIRE(uv::unwrapIslandToUv(mesh, island));

    const Vec2 before = uv::islandUvCentroid(mesh, island);
    uv::translateIslandUv(mesh, island, {5.0f, -2.0f});
    const Vec2 after = uv::islandUvCentroid(mesh, island);
    REQUIRE(after.x == doctest::Approx(before.x + 5.0f));
    REQUIRE(after.y == doctest::Approx(before.y - 2.0f));
}

TEST_CASE("autoSeams partitions a cube into one chart per face") {
    Mesh mesh = makeCube();
    const uv::SeamSet seams = uv::autoSeams(mesh);
    // Six faces, each its own normal-coherent chart -> every one of the 12 cube
    // edges lies between two different charts and is seamed.
    REQUIRE(seams.size() == 12);
    const auto islands = uv::computeIslands(mesh, seams);
    REQUIRE(islands.size() == 6);
    for (const auto& island : islands) {
        REQUIRE(island.size() == 1);
    }
}

TEST_CASE("unwrapAtlas produces a low-distortion, in-bounds cube atlas") {
    Mesh mesh = makeCube();
    const uv::AtlasResult atlas = uv::unwrapAtlas(mesh);

    REQUIRE(atlas.ok);
    REQUIRE(atlas.chartCount == 6);
    REQUIRE(atlas.flippedCharts == 0);
    // Each chart is a single planar quad, so LSCM is near-perfectly conformal.
    REQUIRE(atlas.maxAngleDistortion < 1e-3f);

    // Every corner UV must land inside the unit square with no chart overlap.
    const std::vector<Vec2>* uvs = uv::uvColumn(mesh);
    REQUIRE(uvs != nullptr);
    for (const Vec2& p : *uvs) {
        REQUIRE(p.x >= -1e-5f);
        REQUIRE(p.y >= -1e-5f);
        REQUIRE(p.x <= 1.0f + 1e-5f);
        REQUIRE(p.y <= 1.0f + 1e-5f);
    }
}

TEST_CASE("unwrapAtlas is deterministic") {
    Mesh a = makeCube();
    Mesh b = makeCube();
    const uv::AtlasResult ra = uv::unwrapAtlas(a);
    const uv::AtlasResult rb = uv::unwrapAtlas(b);
    REQUIRE(ra.chartCount == rb.chartCount);
    REQUIRE(ra.seamEdges == rb.seamEdges);
    const std::vector<Vec2>* ua = uv::uvColumn(a);
    const std::vector<Vec2>* ub = uv::uvColumn(b);
    REQUIRE(ua != nullptr);
    REQUIRE(ub != nullptr);
    REQUIRE(ua->size() == ub->size());
    for (std::size_t i = 0; i < ua->size(); ++i) {
        REQUIRE((*ua)[i].x == doctest::Approx((*ub)[i].x));
        REQUIRE((*ua)[i].y == doctest::Approx((*ub)[i].y));
    }
}

TEST_CASE("mirrored UVs are detected as a flipped island") {
    Mesh mesh = makeQuad(1.0f, 1.0f);
    const std::vector<FaceId> island = aliveFaces(mesh);
    REQUIRE(uv::unwrapIslandToUv(mesh, island));

    REQUIRE_FALSE(uv::measureDistortion(mesh, island).flipped);
    // Mirror U about x=0 to reverse winding.
    uv::scaleIslandUv(mesh, island, {-1.0f, 1.0f}, {0.0f, 0.0f});
    REQUIRE(uv::measureDistortion(mesh, island).flipped);
}
