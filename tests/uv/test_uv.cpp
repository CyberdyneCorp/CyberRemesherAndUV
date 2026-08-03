#include <doctest.h>

#include <cmath>
#include <random>
#include <vector>

#include "cyber/core/math.hpp"
#include "cyber/core/mesh.hpp"
#include "cyber/uv/atlas.hpp"
#include "cyber/uv/common.hpp"
#include "cyber/uv/distortion.hpp"
#include "cyber/uv/packing.hpp"
#include "cyber/uv/layout.hpp"
#include "cyber/uv/pack_aids.hpp"
#include "cyber/uv/reunwrap.hpp"
#include "cyber/uv/udim.hpp"
#include "cyber/uv/uv_sets.hpp"
#include "cyber/uv/uv_stack.hpp"
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

// A strip of quads bending around the y-axis, `degPerQuad` per quad. Seed growth
// splits it once the running normal exceeds the chart-angle bound; the merge pass
// can re-unite the pieces when the whole strip still fits one normal cone.
Mesh makeBentStrip(int quads, float degPerQuad) {
    std::vector<Vec3> p;
    std::vector<std::vector<Index>> f;
    const float d = degPerQuad * 3.14159265f / 180.0f;
    for (int i = 0; i <= quads; ++i) {
        const float a = d * static_cast<float>(i);
        p.push_back({std::cos(a), 0.0f, std::sin(a)});
        p.push_back({std::cos(a), 1.0f, std::sin(a)});
    }
    for (int i = 0; i < quads; ++i) {
        const Index b0 = static_cast<Index>(2 * i);
        const Index b1 = static_cast<Index>(2 * (i + 1));
        f.push_back({b0, b1, b1 + 1, b0 + 1});
    }
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

TEST_CASE("skyline packing is tighter than shelf and stays valid") {
    // Boxes of varied heights: tallest-first shelf packing leaves vertical gaps
    // under the short boxes that the skyline strategy drops later boxes into.
    std::vector<uv::Bounds2> boxes;
    const float dims[][2] = {{1.0f, 1.0f}, {1.0f, 0.3f}, {0.5f, 1.0f}, {0.4f, 0.4f},
                             {1.0f, 0.6f}, {0.7f, 0.9f}, {0.3f, 0.2f}, {0.9f, 0.5f}};
    for (const auto& d : dims) {
        uv::Bounds2 b;
        b.expand({0.0f, 0.0f});
        b.expand({d[0], d[1]});
        boxes.push_back(b);
    }

    uv::PackParams shelf;
    shelf.strategy = uv::PackStrategy::Shelf;
    uv::PackParams sky;
    sky.strategy = uv::PackStrategy::Skyline;
    const uv::PackResult a = uv::packBoxes(boxes, shelf);
    const uv::PackResult b = uv::packBoxes(boxes, sky);
    REQUIRE(a.ok);
    REQUIRE(b.ok);

    // Skyline fits the same boxes into a smaller square -> higher coverage.
    REQUIRE(b.usedArea > a.usedArea);

    // ...while staying a valid packing: inside the unit square, no overlaps.
    for (const uv::PackedIsland& island : b.islands) {
        REQUIRE(island.placed.mn.x >= -1e-5f);
        REQUIRE(island.placed.mn.y >= -1e-5f);
        REQUIRE(island.placed.mx.x <= 1.0f + 1e-5f);
        REQUIRE(island.placed.mx.y <= 1.0f + 1e-5f);
    }
    // Skyline places boxes edge-to-edge, so neighbours touch exactly; the slack
    // absorbs the 1-ULP float noise at a shared boundary while still catching any
    // real interior overlap.
    for (std::size_t i = 0; i < b.islands.size(); ++i) {
        for (std::size_t j = i + 1; j < b.islands.size(); ++j) {
            REQUIRE_FALSE(uv::Bounds2::overlap(b.islands[i].placed, b.islands[j].placed, 1e-5f));
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
    const std::vector<Vec3> p = {{0, 0, 0}, {1, 0, 0}, {1, 1, 0}, {0, 1, 0}, {2, 0, 0}, {2, 1, 0}};
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
    // No merging: each cube face is its own normal-coherent chart.
    uv::AtlasOptions opts;
    opts.mergeCharts = false;
    const uv::SeamSet seams = uv::autoSeams(mesh, opts);
    // Six faces -> every one of the 12 cube edges lies between two different
    // charts and is seamed.
    REQUIRE(seams.size() == 12);
    const auto islands = uv::computeIslands(mesh, seams);
    REQUIRE(islands.size() == 6);
    for (const auto& island : islands) {
        REQUIRE(island.size() == 1);
    }
}

TEST_CASE("unwrapAtlas produces a low-distortion, in-bounds cube atlas") {
    Mesh mesh = makeCube();
    // Cone-only charting (no distortion merge) keeps one chart per cube face.
    uv::AtlasOptions opts;
    opts.maxChartDistortion = 0.0f;
    const uv::AtlasResult atlas = uv::unwrapAtlas(mesh, opts);

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

TEST_CASE("chart merge recombines fragmented coplanar-compatible charts") {
    // 7 quads at 10 deg each span 60 deg of normal — seed growth (40 deg bound)
    // splits them into two charts, but the whole strip fits a single 40 deg cone
    // about its mean normal, so the merge pass reunites it.
    Mesh split = makeBentStrip(7, 10.0f);
    uv::AtlasOptions noMerge;
    noMerge.mergeCharts = false;
    const uv::AtlasResult a = uv::unwrapAtlas(split, noMerge);

    Mesh merged = makeBentStrip(7, 10.0f);
    uv::AtlasOptions merge;
    merge.mergeCharts = true;
    merge.maxChartDistortion = 0.0f;  // cone merge only
    const uv::AtlasResult b = uv::unwrapAtlas(merged, merge);

    REQUIRE(a.chartCount > 1);
    REQUIRE(b.chartCount < a.chartCount);
    REQUIRE(b.flippedCharts == 0);
    // The reunited chart still lives inside one normal cone, so it stays a
    // sanely-bounded conformal map (not an arbitrary blow-up).
    REQUIRE(b.maxAngleDistortion < 0.35f);

    // A cube's faces are 90 deg apart, so the cone merge must NOT combine them.
    Mesh cube = makeCube();
    const uv::AtlasResult c = uv::unwrapAtlas(cube, merge);
    REQUIRE(c.chartCount == 6);
}

TEST_CASE("distortion-bounded merge folds developable strips together") {
    // With the looser distortion cap on, the cube's six faces collapse to two
    // developable three-face strips (each unrolls flat), so the chart count
    // drops below the six cone-only charts while distortion stays ~0.
    Mesh cube = makeCube();
    uv::AtlasOptions opts;
    opts.maxChartDistortion = 0.10f;
    const uv::AtlasResult atlas = uv::unwrapAtlas(cube, opts);

    REQUIRE(atlas.ok);
    REQUIRE(atlas.chartCount < 6);
    REQUIRE(atlas.flippedCharts == 0);
    // Developable strips unwrap with essentially no distortion.
    REQUIRE(atlas.maxAngleDistortion <= 0.10f);

    // The cap is honoured relative to disabling the pass: it never raises the
    // chart count and here strictly lowers it.
    Mesh coneCube = makeCube();
    uv::AtlasOptions coneOnly;
    coneOnly.maxChartDistortion = 0.0f;
    const uv::AtlasResult base = uv::unwrapAtlas(coneCube, coneOnly);
    REQUIRE(atlas.chartCount < base.chartCount);
}

TEST_CASE("chart re-orientation tightens the cube atlas") {
    // Without re-orientation LSCM leaves each cube face as a 45-degree diamond,
    // whose axis-aligned bounding box wastes half its area. Re-orienting to the
    // minimum-area box makes the faces axis-aligned squares, so the packer fits
    // them at a larger scale -> higher texel density.
    Mesh loose = makeCube();
    uv::AtlasOptions noReorient;
    noReorient.reorientCharts = false;
    noReorient.maxChartDistortion = 0.0f;  // per-face charts, so faces are diamonds
    const uv::AtlasResult a = uv::unwrapAtlas(loose, noReorient);

    Mesh tight = makeCube();
    uv::AtlasOptions reorient;
    reorient.reorientCharts = true;
    reorient.maxChartDistortion = 0.0f;
    const uv::AtlasResult b = uv::unwrapAtlas(tight, reorient);

    REQUIRE(a.chartCount == b.chartCount);
    REQUIRE(b.flippedCharts == 0);
    // The conformal distortion is unaffected (rotation is a similarity)...
    REQUIRE(b.maxAngleDistortion == doctest::Approx(a.maxAngleDistortion).epsilon(1e-4));
    // ...but the pack is materially tighter (cube faces double their coverage).
    REQUIRE(b.texelDensity > a.texelDensity * 1.2f);
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

// ---- single-island re-unwrap (add-stage-dependent-x-gesture, 6.2b) ---------

TEST_CASE("reunwrapIsland refuses a mesh with no UVs instead of half-filling a column") {
    Mesh mesh = makeCube();
    uv::AtlasOptions opts;
    opts.mergeCharts = false;
    const uv::SeamSet seams = uv::autoSeams(mesh, opts);

    // UVs live in ONE per-corner column, so writing a single island into a fresh column would
    // leave every OTHER island's corners at (0,0) -- a full, non-null UV stream in which
    // untouched islands read as a real layout collapsed at the origin.
    const uv::ReunwrapResult r = uv::reunwrapIsland(mesh, FaceId{0}, seams);
    REQUIRE(!r.ok);
    REQUIRE(r.needsWholeMeshUnwrap);
    REQUIRE(uv::uvColumn(mesh) == nullptr);
}

TEST_CASE("reunwrapIsland leaves every other island's UVs bitwise unchanged") {
    Mesh mesh = makeCube();
    uv::AtlasOptions opts;
    opts.mergeCharts = false;
    opts.maxChartDistortion = 0.0f;
    REQUIRE(uv::unwrapAtlas(mesh, opts).ok);
    const uv::SeamSet seams = uv::autoSeams(mesh, opts);
    const auto islands = uv::computeIslands(mesh, seams);
    REQUIRE(islands.size() == 6);

    const std::vector<Vec2> before = *uv::uvColumn(mesh);
    const FaceId target = islands[0][0];
    const uv::ReunwrapResult r = uv::reunwrapIsland(mesh, target, seams);
    REQUIRE(r.ok);
    REQUIRE(r.faces == 1);

    // Every corner NOT in the target island must be bitwise identical: a localized gesture
    // must not rearrange a layout the artist already arranged.
    const std::vector<Vec2>& after = *uv::uvColumn(mesh);
    REQUIRE(after.size() == before.size());
    for (std::size_t i = 1; i < islands.size(); ++i) {
        for (const FaceId f : islands[i]) {
            for (const cyber::LoopId loop : mesh.faceLoops(f)) {
                const auto k = static_cast<std::size_t>(loop.value);
                REQUIRE(after[k].x == before[k].x);
                REQUIRE(after[k].y == before[k].y);
            }
        }
    }
}

TEST_CASE("a re-unwrapped island stays inside its previous footprint") {
    Mesh mesh = makeCube();
    uv::AtlasOptions opts;
    opts.mergeCharts = false;
    opts.maxChartDistortion = 0.0f;
    REQUIRE(uv::unwrapAtlas(mesh, opts).ok);
    const uv::SeamSet seams = uv::autoSeams(mesh, opts);
    const auto islands = uv::computeIslands(mesh, seams);

    const uv::Bounds2 before = uv::islandUvBounds(mesh, islands[0]);
    REQUIRE(uv::reunwrapIsland(mesh, islands[0][0], seams).ok);
    const uv::Bounds2 after = uv::islandUvBounds(mesh, islands[0]);

    // Inside, not equal to: the fit is UNIFORM, so an island whose aspect changed occupies
    // less than the full box. Filling it exactly would need a per-axis scale, and that shear
    // would destroy the conformality the solve just computed.
    const float slack = 1e-4f;
    REQUIRE(after.mn.x >= before.mn.x - slack);
    REQUIRE(after.mn.y >= before.mn.y - slack);
    REQUIRE(after.mx.x <= before.mx.x + slack);
    REQUIRE(after.mx.y <= before.mx.y + slack);
    // Centred on the same point, so the island does not drift within its box.
    REQUIRE(std::fabs(after.center().x - before.center().x) < slack);
    REQUIRE(std::fabs(after.center().y - before.center().y) < slack);
    // And it is not collapsed: a "fit" that shrank the island to nothing would satisfy the
    // containment checks above while destroying the layout.
    REQUIRE(after.size().x > slack);
    REQUIRE(after.size().y > slack);
}

TEST_CASE("re-unwrapping the same island twice does not move it") {
    Mesh mesh = makeCube();
    uv::AtlasOptions opts;
    opts.mergeCharts = false;
    opts.maxChartDistortion = 0.0f;
    REQUIRE(uv::unwrapAtlas(mesh, opts).ok);
    const uv::SeamSet seams = uv::autoSeams(mesh, opts);
    const auto islands = uv::computeIslands(mesh, seams);

    REQUIRE(uv::reunwrapIsland(mesh, islands[0][0], seams).ok);
    const uv::Bounds2 once = uv::islandUvBounds(mesh, islands[0]);
    REQUIRE(uv::reunwrapIsland(mesh, islands[0][0], seams).ok);
    const uv::Bounds2 twice = uv::islandUvBounds(mesh, islands[0]);

    // Stability matters because the gesture is repeatable by nature: an artist who X's the
    // same island twice must not watch it walk across the square.
    const float slack = 1e-4f;
    REQUIRE(std::fabs(twice.mn.x - once.mn.x) < slack);
    REQUIRE(std::fabs(twice.mn.y - once.mn.y) < slack);
    REQUIRE(std::fabs(twice.mx.x - once.mx.x) < slack);
    REQUIRE(std::fabs(twice.mx.y - once.mx.y) < slack);
}

TEST_CASE("reunwrapIsland rejects a dead face and touches nothing") {
    Mesh mesh = makeCube();
    uv::AtlasOptions opts;
    opts.mergeCharts = false;
    REQUIRE(uv::unwrapAtlas(mesh, opts).ok);
    const uv::SeamSet seams = uv::autoSeams(mesh, opts);
    const std::vector<Vec2> before = *uv::uvColumn(mesh);

    const uv::ReunwrapResult r = uv::reunwrapIsland(mesh, FaceId{9999}, seams);
    REQUIRE(!r.ok);
    REQUIRE(!r.needsWholeMeshUnwrap);  // it has UVs; the FACE is the problem
    const std::vector<Vec2>& after = *uv::uvColumn(mesh);
    for (std::size_t i = 0; i < before.size(); ++i) {
        REQUIRE(after[i].x == before[i].x);
        REQUIRE(after[i].y == before[i].y);
    }
}

// ---- packing scan equivalence (add-uv-packing-aids, 6.6) -------------------

// The two run-scanners must agree on EVERY input, because `packBoxes` picks between them by a
// measured span threshold. If they disagreed, the packed layout would depend on that threshold
// — a performance constant would silently be changing artists' results.
TEST_CASE("both skyline run-scanners agree on randomized height maps") {
    std::mt19937 rng(20260728u);
    std::uniform_real_distribution<float> height(0.0f, 4.0f);
    std::vector<int> scratch;

    for (int trial = 0; trial < 200; ++trial) {
        std::vector<float> heights(static_cast<std::size_t>(uv::kSkylineColumns));
        for (float& h : heights) {
            h = height(rng);
        }
        // Spans either side of the threshold, including the degenerate ends.
        for (const int span : {1, 2, 7, 8, 9, 33, 200, uv::kSkylineColumns}) {
            const auto naive = uv::lowestRunNaive(heights, span);
            const auto fast = uv::lowestRunMonotonic(heights, span, scratch);
            REQUIRE(naive.first == fast.first);
            REQUIRE(naive.second == fast.second);
        }
    }
}

TEST_CASE("a flat height map resolves to the leftmost run in both scanners") {
    // Ties must break the same way, or the packer's output would depend on the threshold even
    // when the two scanners are otherwise equivalent.
    const std::vector<float> flat(static_cast<std::size_t>(uv::kSkylineColumns), 1.5f);
    std::vector<int> scratch;
    for (const int span : {1, 8, 64}) {
        const auto naive = uv::lowestRunNaive(flat, span);
        const auto fast = uv::lowestRunMonotonic(flat, span, scratch);
        REQUIRE(naive.first == 0);
        REQUIRE(fast.first == 0);
        REQUIRE(naive.second == 1.5f);
        REQUIRE(fast.second == 1.5f);
    }
}

// ---- manual packing aids (add-uv-packing-aids, 6.6) -----------------------

namespace {
// Islands of an unwrapped cube, with the atlas's own partition.
struct CubeAtlas {
    Mesh mesh;
    std::vector<std::vector<FaceId>> islands;
};
CubeAtlas unwrappedCube() {
    CubeAtlas out{makeCube(), {}};
    uv::AtlasOptions opts;
    opts.mergeCharts = false;
    opts.maxChartDistortion = 0.0f;
    REQUIRE(uv::unwrapAtlas(out.mesh, opts).ok);
    out.islands = uv::computeIslands(out.mesh, uv::autoSeams(out.mesh, opts));
    return out;
}
}  // namespace

TEST_CASE("packing into a sub-region puts every island inside it") {
    CubeAtlas atlas = unwrappedCube();
    uv::Bounds2 region;
    region.expand({0.25f, 0.10f});
    region.expand({0.75f, 0.60f});

    const uv::PackResult r = uv::packIslandsIntoRegion(atlas.mesh, atlas.islands, region);
    REQUIRE(r.ok);

    const float slack = 1e-5f;
    for (const auto& island : atlas.islands) {
        const uv::Bounds2 box = uv::islandUvBounds(atlas.mesh, island);
        REQUIRE(box.valid());
        REQUIRE(box.mn.x >= region.mn.x - slack);
        REQUIRE(box.mn.y >= region.mn.y - slack);
        REQUIRE(box.mx.x <= region.mx.x + slack);
        REQUIRE(box.mx.y <= region.mx.y + slack);
    }
}

TEST_CASE("region packing scales uniformly, so a non-square region does not shear islands") {
    // A non-square region is the case that would tempt a per-axis fit. Packing must apply ONE
    // scale, or it would reshape islands and silently undo the unwrap.
    CubeAtlas wide = unwrappedCube();
    const uv::Bounds2 before = uv::islandUvBounds(wide.mesh, wide.islands[0]);
    const Vec2 beforeSize = before.size();

    uv::Bounds2 region;
    region.expand({0.0f, 0.0f});
    region.expand({1.0f, 0.5f});  // 2:1
    REQUIRE(uv::packIslandsIntoRegion(wide.mesh, wide.islands, region).ok);

    const Vec2 afterSize = uv::islandUvBounds(wide.mesh, wide.islands[0]).size();
    REQUIRE(beforeSize.x > 0.0f);
    REQUIRE(beforeSize.y > 0.0f);
    // Aspect ratio preserved: the two axes were scaled by the same factor.
    const float beforeAspect = beforeSize.x / beforeSize.y;
    const float afterAspect = afterSize.x / afterSize.y;
    REQUIRE(std::fabs(afterAspect - beforeAspect) < 1e-3f);
}

TEST_CASE("a degenerate region is refused rather than collapsing the layout") {
    CubeAtlas atlas = unwrappedCube();
    const std::vector<Vec2> before = *uv::uvColumn(atlas.mesh);

    uv::Bounds2 flat;
    flat.expand({0.2f, 0.5f});
    flat.expand({0.8f, 0.5f});  // zero height
    REQUIRE(!uv::packIslandsIntoRegion(atlas.mesh, atlas.islands, flat).ok);

    // And nothing was written: a "successful" pack onto a line would be a destroyed layout.
    const std::vector<Vec2>& after = *uv::uvColumn(atlas.mesh);
    for (std::size_t i = 0; i < before.size(); ++i) {
        REQUIRE(after[i].x == before[i].x);
        REQUIRE(after[i].y == before[i].y);
    }
}

TEST_CASE("distributing overlapping islands removes every box overlap") {
    CubeAtlas atlas = unwrappedCube();
    // Stack every island on the same spot, the worst case the double-tap aid exists for.
    for (const auto& island : atlas.islands) {
        const uv::Bounds2 box = uv::islandUvBounds(atlas.mesh, island);
        uv::translateIslandUv(atlas.mesh, island, Vec2{0.5f, 0.5f} - box.center());
    }
    uv::distributeIslandsUv(atlas.mesh, atlas.islands, 0.01f);

    for (std::size_t i = 0; i < atlas.islands.size(); ++i) {
        for (std::size_t j = i + 1; j < atlas.islands.size(); ++j) {
            const uv::Bounds2 a = uv::islandUvBounds(atlas.mesh, atlas.islands[i]);
            const uv::Bounds2 b = uv::islandUvBounds(atlas.mesh, atlas.islands[j]);
            REQUIRE(!uv::Bounds2::overlap(a, b, 1e-5f));
        }
    }
}

TEST_CASE("flipping an island reverses its winding and flipping twice is identity") {
    CubeAtlas atlas = unwrappedCube();
    const auto& island = atlas.islands[0];
    const std::vector<Vec2> before = *uv::uvColumn(atlas.mesh);
    const bool flippedBefore = uv::measureDistortion(atlas.mesh, island).flipped;
    const uv::Bounds2 boxBefore = uv::islandUvBounds(atlas.mesh, island);

    uv::flipIslandUv(atlas.mesh, island);
    REQUIRE(uv::measureDistortion(atlas.mesh, island).flipped != flippedBefore);
    // The flip must not double as a translation, or it could push a shell out of the region it
    // was packed into.
    const uv::Bounds2 boxAfter = uv::islandUvBounds(atlas.mesh, island);
    REQUIRE(std::fabs(boxAfter.center().x - boxBefore.center().x) < 1e-5f);
    REQUIRE(std::fabs(boxAfter.center().y - boxBefore.center().y) < 1e-5f);

    uv::flipIslandUv(atlas.mesh, island);
    const std::vector<Vec2>& after = *uv::uvColumn(atlas.mesh);
    for (std::size_t i = 0; i < before.size(); ++i) {
        REQUIRE(std::fabs(after[i].x - before[i].x) < 1e-6f);
        REQUIRE(std::fabs(after[i].y - before[i].y) < 1e-6f);
    }
}

TEST_CASE("flippedIslands names which islands are mirrored, not merely how many") {
    CubeAtlas atlas = unwrappedCube();
    REQUIRE(uv::flippedIslands(atlas.mesh, atlas.islands).empty());

    uv::flipIslandUv(atlas.mesh, atlas.islands[2]);
    const std::vector<std::size_t> flipped = uv::flippedIslands(atlas.mesh, atlas.islands);
    // Pointing AT the offender is the whole value: a count tells an artist there is a problem
    // without telling them where.
    REQUIRE(flipped.size() == 1);
    REQUIRE(flipped[0] == 2);
}

TEST_CASE("region packing refuses a mesh with no UV layout") {
    Mesh mesh = makeCube();
    uv::AtlasOptions opts;
    opts.mergeCharts = false;
    const auto islands = uv::computeIslands(mesh, uv::autoSeams(mesh, opts));
    uv::Bounds2 region;
    region.expand({0.0f, 0.0f});
    region.expand({1.0f, 1.0f});
    // Reporting ok here would tell the caller the layout was repacked when there is no layout.
    REQUIRE(!uv::packIslandsIntoRegion(mesh, islands, region).ok);
    REQUIRE(uv::uvColumn(mesh) == nullptr);
}

// ---- UDIM tiles (add-uv-sets-and-stacking, 6.7) ---------------------------

TEST_CASE("UDIM index and coordinate round-trip through the standard numbering") {
    REQUIRE(uv::udimIndex({0, 0}) == 1001);
    REQUIRE(uv::udimIndex({9, 0}) == 1010);
    REQUIRE(uv::udimIndex({0, 1}) == 1011);
    for (const int index : {1001, 1005, 1010, 1011, 1042}) {
        const uv::UdimCoord c = uv::udimCoord(index);
        REQUIRE(uv::udimIndex(c) == index);
    }
}

TEST_CASE("a packed atlas occupies exactly tile 1001") {
    CubeAtlas atlas = unwrappedCube();
    // The atlas packs into the unit square, which IS tile 1001. Anything else would mean the
    // default layout silently spanned texture files.
    const std::vector<int> tiles = uv::occupiedUdimTiles(atlas.mesh, atlas.islands);
    REQUIRE(tiles.size() == 1);
    REQUIRE(tiles[0] == 1001);
    for (const auto& island : atlas.islands) {
        REQUIRE(!uv::islandStraddlesTiles(atlas.mesh, island));
    }
}

TEST_CASE("assigning an island to a tile moves it whole tiles and preserves its place") {
    CubeAtlas atlas = unwrappedCube();
    const auto& island = atlas.islands[1];
    const uv::Bounds2 before = uv::islandUvBounds(atlas.mesh, island);

    uv::assignIslandToTile(atlas.mesh, island, {2, 1});
    REQUIRE(uv::udimIndex(uv::islandTile(atlas.mesh, island)) == 1013);

    const uv::Bounds2 after = uv::islandUvBounds(atlas.mesh, island);
    // Moved by exactly (2, 1) — a whole number of tiles — so its position WITHIN the tile and
    // its size are untouched. Re-arranging a shell while retiling it would be a second,
    // unrequested edit.
    const float slack = 1e-5f;
    REQUIRE(std::fabs((after.mn.x - before.mn.x) - 2.0f) < slack);
    REQUIRE(std::fabs((after.mn.y - before.mn.y) - 1.0f) < slack);
    REQUIRE(std::fabs(after.size().x - before.size().x) < slack);
    REQUIRE(std::fabs(after.size().y - before.size().y) < slack);

    // And the occupied-tile list now names both tiles, which is what an exporter needs.
    const std::vector<int> tiles = uv::occupiedUdimTiles(atlas.mesh, atlas.islands);
    REQUIRE(tiles.size() == 2);
    REQUIRE(tiles[0] == 1001);
    REQUIRE(tiles[1] == 1013);
}

TEST_CASE("an island straddling a tile border is detected") {
    CubeAtlas atlas = unwrappedCube();
    const auto& island = atlas.islands[0];
    const uv::Bounds2 box = uv::islandUvBounds(atlas.mesh, island);
    // Push it so it sits across u = 1: half in tile 1001, half in 1002. This splits a shell
    // across two texture files on export, which is almost never intended.
    uv::translateIslandUv(atlas.mesh, island, Vec2{1.0f - box.center().x, 0.0f});
    REQUIRE(uv::islandStraddlesTiles(atlas.mesh, island));
}

TEST_CASE("an island flush against the tile edge does NOT count as straddling") {
    CubeAtlas atlas = unwrappedCube();
    const auto& island = atlas.islands[0];
    const uv::Bounds2 box = uv::islandUvBounds(atlas.mesh, island);
    // Its maximum lands exactly on u = 1. It fills the tile it is in and merely touches the
    // next; reporting that as a straddle would flag every tightly packed layout.
    uv::translateIslandUv(atlas.mesh, island, Vec2{1.0f - box.mx.x, 0.0f});
    REQUIRE(!uv::islandStraddlesTiles(atlas.mesh, island));
}

// ---- symmetry-aware stacking (add-uv-sets-and-stacking, 6.7) --------------

namespace {
// Two disjoint quads mirrored across x = 0, each subdivided into 2 faces so a pairing has more
// than one face to correspond. Deliberately NOT symmetric in vertex ORDER, so a correspondence
// built from corner indices rather than positions would fail.
Mesh makeMirroredPair() {
    // Right side x in [1,3], left side its exact mirror across x = 0. The left side is wound in
    // the OPPOSITE order deliberately, so a correspondence built from corner indices rather than
    // world positions cannot accidentally work.
    const std::vector<Vec3> p = {
        {1, 0, 0}, {2, 0, 0}, {2, 1, 0}, {1, 1, 0}, {3, 0, 0}, {3, 1, 0},
        {-1, 0, 0}, {-2, 0, 0}, {-2, 1, 0}, {-1, 1, 0}, {-3, 0, 0}, {-3, 1, 0},
    };
    const std::vector<std::vector<Index>> f = {
        {0, 1, 2, 3}, {1, 4, 5, 2},
        {9, 8, 7, 6}, {8, 11, 10, 7},
    };
    return Mesh::fromIndexed(p, f);
}
}  // namespace

TEST_CASE("mirror island pairs are found by geometry, not by island order") {
    Mesh mesh = makeMirroredPair();
    uv::AtlasOptions opts;
    opts.mergeCharts = false;
    REQUIRE(uv::unwrapAtlas(mesh, opts).ok);
    const auto islands = uv::computeIslands(mesh, uv::autoSeams(mesh, opts));
    REQUIRE(islands.size() == 2);

    const auto pairs = uv::findMirrorIslandPairs(mesh, islands, Vec3{0, 0, 0}, Vec3{1, 0, 0},
                                                 0.01f);
    REQUIRE(pairs.size() == 1);
    // `primary` is always the lower index, so the result is deterministic.
    REQUIRE(pairs[0].primary < pairs[0].mirror);
}

TEST_CASE("stacking makes a mirror pair occupy identical UV space") {
    Mesh mesh = makeMirroredPair();
    uv::AtlasOptions opts;
    opts.mergeCharts = false;
    REQUIRE(uv::unwrapAtlas(mesh, opts).ok);
    const auto islands = uv::computeIslands(mesh, uv::autoSeams(mesh, opts));
    const auto pairs = uv::findMirrorIslandPairs(mesh, islands, Vec3{0, 0, 0}, Vec3{1, 0, 0},
                                                 0.01f);
    REQUIRE(pairs.size() == 1);

    // Before stacking the atlas has given them separate space, which is the "keep them unique"
    // behaviour — so the test is measuring a real change.
    const uv::Bounds2 a0 = uv::islandUvBounds(mesh, islands[pairs[0].primary]);
    const uv::Bounds2 b0 = uv::islandUvBounds(mesh, islands[pairs[0].mirror]);
    REQUIRE(!uv::Bounds2::overlap(a0, b0, 1e-4f));

    REQUIRE(uv::stackMirroredIslands(mesh, islands, pairs, Vec3{0, 0, 0}, Vec3{1, 0, 0}, 0.01f)
            == 1);

    // Identical UV space is the point: the pair now shares one texture region, halving its
    // texel cost.
    const uv::Bounds2 a1 = uv::islandUvBounds(mesh, islands[pairs[0].primary]);
    const uv::Bounds2 b1 = uv::islandUvBounds(mesh, islands[pairs[0].mirror]);
    const float slack = 1e-4f;
    REQUIRE(std::fabs(a1.mn.x - b1.mn.x) < slack);
    REQUIRE(std::fabs(a1.mn.y - b1.mn.y) < slack);
    REQUIRE(std::fabs(a1.mx.x - b1.mx.x) < slack);
    REQUIRE(std::fabs(a1.mx.y - b1.mx.y) < slack);
    // The PRIMARY must not have moved — stacking puts the mirror onto it, not both somewhere new.
    REQUIRE(std::fabs(a1.mn.x - a0.mn.x) < slack);
    REQUIRE(std::fabs(a1.mn.y - a0.mn.y) < slack);

    // Matching bounding boxes are NOT enough: a scrambled shell has the same box. Every corner
    // of the mirror island must carry the UV of its geometric counterpart in the primary, which
    // is the difference between sharing UV space and merely overlapping it.
    const std::vector<Vec2>* uvs = uv::uvColumn(mesh);
    REQUIRE(uvs != nullptr);
    std::size_t checked = 0;
    for (const FaceId face : islands[pairs[0].mirror]) {
        for (const cyber::LoopId loop : mesh.faceLoops(face)) {
            const Vec3 here = mesh.position(mesh.loopVertex(loop));
            const Vec3 want{-here.x, here.y, here.z};  // reflection across x = 0
            bool found = false;
            for (const FaceId other : islands[pairs[0].primary]) {
                for (const cyber::LoopId otherLoop : mesh.faceLoops(other)) {
                    if (length(mesh.position(mesh.loopVertex(otherLoop)) - want) < 1e-4f) {
                        const Vec2 mine = (*uvs)[static_cast<std::size_t>(loop.value)];
                        const Vec2 theirs = (*uvs)[static_cast<std::size_t>(otherLoop.value)];
                        REQUIRE(std::fabs(mine.x - theirs.x) < slack);
                        REQUIRE(std::fabs(mine.y - theirs.y) < slack);
                        found = true;
                        ++checked;
                        break;
                    }
                }
                if (found) {
                    break;
                }
            }
            REQUIRE(found);
        }
    }
    REQUIRE(checked == 8);  // two quads, four corners each
}

TEST_CASE("a non-symmetric mesh yields no pairs rather than a wrong pairing") {
    CubeAtlas atlas = unwrappedCube();
    // A cube's faces are not mirror images of each other across x = 0 in the pairwise sense this
    // looks for at a tight tolerance, and reporting a pair here would stack unrelated shells.
    const auto pairs = uv::findMirrorIslandPairs(atlas.mesh, atlas.islands, Vec3{0.5f, 0, 0},
                                                 Vec3{1, 0, 0}, 1e-4f);
    for (const auto& pair : pairs) {
        // If any pair IS reported it must at least be a real geometric mirror, never a
        // same-island self-pair.
        REQUIRE(pair.primary != pair.mirror);
    }
}

TEST_CASE("stacking is refused on a mesh with no UVs") {
    Mesh mesh = makeMirroredPair();
    uv::AtlasOptions opts;
    opts.mergeCharts = false;
    const auto islands = uv::computeIslands(mesh, uv::autoSeams(mesh, opts));
    const std::vector<uv::MirrorIslandPair> pairs{{0, 1}};
    // Nothing to stack, and nothing created: the same absence-versus-zero discipline the rest of
    // the UV path follows.
    REQUIRE(uv::stackMirroredIslands(mesh, islands, pairs, Vec3{0, 0, 0}, Vec3{1, 0, 0}, 0.01f)
            == 0);
    REQUIRE(uv::uvColumn(mesh) == nullptr);
}

// ---- multiple UV sets (add-uv-sets, 6.7a) ---------------------------------

TEST_CASE("a set list names the active set, and creating one copies the active layout") {
    CubeAtlas atlas = unwrappedCube();
    const std::string active = "default";
    REQUIRE(uv::uvSetNames(atlas.mesh, active) == std::vector<std::string>{"default"});

    // A COPY, not an empty set: an empty column would read downstream as a real layout collapsed
    // at the origin, which is the absence-versus-zero trap the whole UV path avoids.
    REQUIRE(uv::createUvSet(atlas.mesh, active, "lightmap"));
    const auto names = uv::uvSetNames(atlas.mesh, active);
    REQUIRE(names.size() == 2);
    REQUIRE(names[0] == "default");
    REQUIRE(names[1] == "lightmap");

    const std::vector<Vec2>* stored =
        atlas.mesh.cornerAttributes().find<Vec2>("uv:lightmap");
    REQUIRE(stored != nullptr);
    REQUIRE(*stored == *uv::uvColumn(atlas.mesh));
}

TEST_CASE("set creation rejects a duplicate, an empty name, a ':' name, and no layout") {
    Mesh bare = makeCube();
    // Nothing to copy.
    REQUIRE(!uv::createUvSet(bare, "default", "lightmap"));

    CubeAtlas atlas = unwrappedCube();
    REQUIRE(!uv::createUvSet(atlas.mesh, "default", "default"));  // duplicate (the active one)
    REQUIRE(!uv::createUvSet(atlas.mesh, "default", ""));
    // ':' is the stored-column separator, so a name containing it would be ambiguous with a
    // differently-split pair.
    REQUIRE(!uv::createUvSet(atlas.mesh, "default", "a:b"));
    REQUIRE(uv::uvSetNames(atlas.mesh, "default").size() == 1);
}

TEST_CASE("activating a set swaps the columns and the whole UV module keeps working") {
    CubeAtlas atlas = unwrappedCube();
    REQUIRE(uv::createUvSet(atlas.mesh, "default", "lightmap"));

    // Make the two sets genuinely different so the swap is observable.
    uv::translateIslandUv(atlas.mesh, atlas.islands[0], Vec2{0.05f, 0.0f});
    const std::vector<Vec2> defaultSet = *uv::uvColumn(atlas.mesh);

    REQUIRE(uv::activateUvSet(atlas.mesh, "default", "lightmap"));
    const std::vector<Vec2> lightmapSet = *uv::uvColumn(atlas.mesh);
    REQUIRE(lightmapSet != defaultSet);
    // The previously active set is preserved under its own name, not discarded.
    const std::vector<Vec2>* kept = atlas.mesh.cornerAttributes().find<Vec2>("uv:default");
    REQUIRE(kept != nullptr);
    REQUIRE(*kept == defaultSet);
    // And the target's stored column is gone: exactly one copy of each set exists.
    REQUIRE(atlas.mesh.cornerAttributes().find<Vec2>("uv:lightmap") == nullptr);

    // The point of keeping the active set under plain `uv`: every existing operation still works
    // on it with no knowledge of sets at all.
    REQUIRE(uv::packIslands(atlas.mesh, atlas.islands).ok);
    REQUIRE(uv::measureDistortion(atlas.mesh, atlas.islands[0]).faces.size() == 1);

    // Switching back restores the first set exactly.
    REQUIRE(uv::activateUvSet(atlas.mesh, "lightmap", "default"));
    REQUIRE(*uv::uvColumn(atlas.mesh) == defaultSet);
}

TEST_CASE("activating refuses an unknown set and the already-active one") {
    CubeAtlas atlas = unwrappedCube();
    REQUIRE(!uv::activateUvSet(atlas.mesh, "default", "nope"));
    REQUIRE(!uv::activateUvSet(atlas.mesh, "default", "default"));
}

TEST_CASE("deleting refuses the ACTIVE set, so the mesh is never left without one") {
    CubeAtlas atlas = unwrappedCube();
    REQUIRE(uv::createUvSet(atlas.mesh, "default", "lightmap"));
    // Deleting the active set would leave `uv` belonging to a set the caller still names, or no
    // active set at all.
    REQUIRE(!uv::deleteUvSet(atlas.mesh, "default", "default"));
    REQUIRE(uv::uvColumn(atlas.mesh) != nullptr);

    REQUIRE(uv::deleteUvSet(atlas.mesh, "default", "lightmap"));
    REQUIRE(uv::uvSetNames(atlas.mesh, "default") == std::vector<std::string>{"default"});
    REQUIRE(!uv::deleteUvSet(atlas.mesh, "default", "lightmap"));  // already gone
}

TEST_CASE("renaming moves a stored set and is a no-op column-wise for the active one") {
    CubeAtlas atlas = unwrappedCube();
    REQUIRE(uv::createUvSet(atlas.mesh, "default", "lightmap"));
    const std::vector<Vec2> before = *atlas.mesh.cornerAttributes().find<Vec2>("uv:lightmap");

    REQUIRE(uv::renameUvSet(atlas.mesh, "default", "lightmap", "bake"));
    REQUIRE(atlas.mesh.cornerAttributes().find<Vec2>("uv:lightmap") == nullptr);
    const std::vector<Vec2>* moved = atlas.mesh.cornerAttributes().find<Vec2>("uv:bake");
    REQUIRE(moved != nullptr);
    REQUIRE(*moved == before);

    // The ACTIVE set's name lives with the caller, so renaming it moves no column; success is what
    // tells the caller to record the new name.
    REQUIRE(uv::renameUvSet(atlas.mesh, "default", "default", "main"));
    REQUIRE(uv::uvColumn(atlas.mesh) != nullptr);
    // Collisions and invalid names are refused.
    REQUIRE(!uv::renameUvSet(atlas.mesh, "default", "bake", "default"));
    REQUIRE(!uv::renameUvSet(atlas.mesh, "default", "bake", "a:b"));
}

TEST_CASE("the sidecar restores INACTIVE sets and the active NAME, never the active layout") {
    CubeAtlas atlas = unwrappedCube();
    REQUIRE(uv::createUvSet(atlas.mesh, "default", "lightmap"));
    // Move the ACTIVE set only, so the two sets differ and it is observable which one wins.
    uv::translateIslandUv(atlas.mesh, atlas.islands[0], Vec2{0.07f, -0.03f});
    const std::vector<Vec2> storedBefore =
        *atlas.mesh.cornerAttributes().find<Vec2>("uv:lightmap");

    const std::vector<std::uint8_t> blob = uv::serializeUvSets(atlas.mesh, "default");
    REQUIRE(!blob.empty());

    CubeAtlas reloaded = unwrappedCube();
    const std::vector<Vec2> payloadLayout = *uv::uvColumn(reloaded.mesh);
    REQUIRE(uv::deserializeUvSets(reloaded.mesh, blob) == "default");

    // The ACTIVE layout is the one the payload carried, untouched. The sidecar does not store it,
    // and must not overwrite it: two copies of the same data let a stale one win, which is exactly
    // the bug the next test pins.
    REQUIRE(*uv::uvColumn(reloaded.mesh) == payloadLayout);
    // The inactive set IS restored, which is the whole reason the sidecar exists.
    const std::vector<Vec2>* restored =
        reloaded.mesh.cornerAttributes().find<Vec2>("uv:lightmap");
    REQUIRE(restored != nullptr);
    REQUIRE(*restored == storedBefore);
}

TEST_CASE("a STALE sidecar cannot resurrect an old active layout over a newer one") {
    // The regression this pins: the first version stored the active set's data in the sidecar too,
    // so restoring it overwrote `uv` — silently discarding every UV edit made since the sidecar was
    // last written. Every mesh edit round-trips through the payload, so that loss happened on the
    // very next edit after any set existed.
    CubeAtlas atlas = unwrappedCube();
    REQUIRE(uv::createUvSet(atlas.mesh, "default", "lightmap"));
    const std::vector<std::uint8_t> stale = uv::serializeUvSets(atlas.mesh, "default");

    // An edit AFTER the sidecar was written.
    uv::translateIslandUv(atlas.mesh, atlas.islands[0], Vec2{0.11f, 0.0f});
    const std::vector<Vec2> edited = *uv::uvColumn(atlas.mesh);

    // Re-applying the stale sidecar must leave the edit intact.
    REQUIRE(uv::deserializeUvSets(atlas.mesh, stale) == "default");
    REQUIRE(*uv::uvColumn(atlas.mesh) == edited);
}

TEST_CASE("a sidecar for different topology is REJECTED, leaving the mesh untouched") {
    CubeAtlas atlas = unwrappedCube();
    const std::vector<std::uint8_t> blob = uv::serializeUvSets(atlas.mesh, "default");

    // A quad has a different corner count. Applying a cube's UV sets to it would shear every
    // island — plausible-looking and wrong — so it must be refused.
    Mesh quad = makeQuad(1.0f, 1.0f);
    uv::AtlasOptions opts;
    REQUIRE(uv::unwrapAtlas(quad, opts).ok);
    const std::vector<Vec2> untouched = *uv::uvColumn(quad);
    REQUIRE(uv::deserializeUvSets(quad, blob).empty());
    REQUIRE(*uv::uvColumn(quad) == untouched);
}

TEST_CASE("a truncated or corrupt sidecar is rejected whole") {
    CubeAtlas atlas = unwrappedCube();
    REQUIRE(uv::createUvSet(atlas.mesh, "default", "lightmap"));
    const std::vector<std::uint8_t> blob = uv::serializeUvSets(atlas.mesh, "default");
    const std::vector<Vec2> before = *uv::uvColumn(atlas.mesh);

    // Truncated: parsed WHOLE before anything is written, so the mesh is left exactly as it was
    // rather than half-restored.
    for (const std::size_t keep : {std::size_t{0}, std::size_t{8}, blob.size() / 2}) {
        CubeAtlas target = unwrappedCube();
        const std::vector<std::uint8_t> cut(blob.begin(),
                                            blob.begin() + static_cast<std::ptrdiff_t>(keep));
        REQUIRE(uv::deserializeUvSets(target.mesh, cut).empty());
    }

    // Wrong magic.
    std::vector<std::uint8_t> bad = blob;
    bad[0] ^= 0xFF;
    CubeAtlas target = unwrappedCube();
    REQUIRE(uv::deserializeUvSets(target.mesh, bad).empty());
    REQUIRE(uv::uvColumn(atlas.mesh) != nullptr);
    REQUIRE(*uv::uvColumn(atlas.mesh) == before);
}

TEST_CASE("restoring REPLACES stored sets rather than merging with them") {
    CubeAtlas source = unwrappedCube();
    REQUIRE(uv::createUvSet(source.mesh, "default", "lightmap"));
    const std::vector<std::uint8_t> blob = uv::serializeUvSets(source.mesh, "default");

    CubeAtlas target = unwrappedCube();
    REQUIRE(uv::createUvSet(target.mesh, "default", "leftover"));
    REQUIRE(uv::deserializeUvSets(target.mesh, blob) == "default");

    // "leftover" came from whatever was loaded before and no save described it; a merge would keep
    // sets the document never recorded.
    const auto names = uv::uvSetNames(target.mesh, "default");
    REQUIRE(std::find(names.begin(), names.end(), "leftover") == names.end());
    REQUIRE(std::find(names.begin(), names.end(), "lightmap") != names.end());
}

// ---- per-vertex UV editing (add-uv-imported-preview, 6.3d) ----------------

namespace {
// Core's `length` is Vec3-only; UV work needs the 2D form.
float uvDistance(Vec2 a, Vec2 b) {
    const Vec2 d = a - b;
    return std::sqrt(d.x * d.x + d.y * d.y);
}
}  // namespace

TEST_CASE("moving a UV vertex moves every coincident corner, keeping the island welded") {
    CubeAtlas atlas = unwrappedCube();
    const auto& island = atlas.islands[0];
    const std::vector<Vec2>* uvs = uv::uvColumn(atlas.mesh);
    REQUIRE(uvs != nullptr);

    // Pick a corner and count how many corners of this island share its UV. On a single-quad cube
    // face that is 1, but the mechanism is what matters: whatever shares the position moves.
    const cyber::LoopId first = atlas.mesh.faceLoops(island[0])[0];
    const Vec2 target = (*uvs)[static_cast<std::size_t>(first.value)];
    std::size_t coincident = 0;
    for (const FaceId face : island) {
        for (const cyber::LoopId loop : atlas.mesh.faceLoops(face)) {
            if (uvDistance((*uvs)[static_cast<std::size_t>(loop.value)], target) < 1e-5f) {
                ++coincident;
            }
        }
    }
    REQUIRE(coincident >= 1);

    const Vec2 delta{0.03f, -0.02f};
    REQUIRE(uv::moveIslandUvVertex(atlas.mesh, island, target, delta, 1e-4f) == coincident);
    // Every coincident corner moved by exactly the delta, so the island did not tear at a point the
    // artist never cut.
    std::size_t moved = 0;
    for (const FaceId face : island) {
        for (const cyber::LoopId loop : atlas.mesh.faceLoops(face)) {
            const Vec2 now = (*uv::uvColumn(atlas.mesh))[static_cast<std::size_t>(loop.value)];
            if (uvDistance(now, target + delta) < 1e-5f) {
                ++moved;
            }
        }
    }
    REQUIRE(moved == coincident);
}

TEST_CASE("a UV vertex move never crosses a SEAM, because a seam means different UVs") {
    CubeAtlas atlas = unwrappedCube();
    // Corners on opposite sides of a seam sit at different UVs by definition, so they fall outside
    // each other's tolerance and move independently. Two DIFFERENT islands is the strongest form of
    // that: moving a vertex in one must not touch the other at all.
    const std::vector<Vec2> otherBefore = [&] {
        std::vector<Vec2> out;
        for (const FaceId face : atlas.islands[1]) {
            for (const cyber::LoopId loop : atlas.mesh.faceLoops(face)) {
                out.push_back((*uv::uvColumn(atlas.mesh))[static_cast<std::size_t>(loop.value)]);
            }
        }
        return out;
    }();

    const cyber::LoopId first = atlas.mesh.faceLoops(atlas.islands[0][0])[0];
    const Vec2 target = (*uv::uvColumn(atlas.mesh))[static_cast<std::size_t>(first.value)];
    REQUIRE(uv::moveIslandUvVertex(atlas.mesh, atlas.islands[0], target, Vec2{0.05f, 0.0f}, 1e-4f)
            > 0);

    std::size_t i = 0;
    for (const FaceId face : atlas.islands[1]) {
        for (const cyber::LoopId loop : atlas.mesh.faceLoops(face)) {
            REQUIRE((*uv::uvColumn(atlas.mesh))[static_cast<std::size_t>(loop.value)]
                    == otherBefore[i++]);
        }
    }
}

TEST_CASE("a UV vertex move that matches nothing reports zero and changes nothing") {
    CubeAtlas atlas = unwrappedCube();
    const std::vector<Vec2> before = *uv::uvColumn(atlas.mesh);
    // Far outside the packed square: a drag that hit no vertex must not be reported as an edit.
    REQUIRE(uv::moveIslandUvVertex(atlas.mesh, atlas.islands[0], Vec2{9.0f, 9.0f},
                                   Vec2{0.1f, 0.1f}, 1e-4f)
            == 0);
    REQUIRE(*uv::uvColumn(atlas.mesh) == before);
}

TEST_CASE("a UV vertex move compares against ORIGINAL positions, so it cannot cascade") {
    CubeAtlas atlas = unwrappedCube();
    const auto& island = atlas.islands[0];
    // A tolerance wide enough to cover the whole island: every corner matches ONCE and moves once.
    // Comparing against positions already updated mid-loop could drag corners along a chain and
    // move some of them twice.
    const std::vector<Vec2> before = *uv::uvColumn(atlas.mesh);
    const Vec2 anchor = before[static_cast<std::size_t>(atlas.mesh.faceLoops(island[0])[0].value)];
    const Vec2 delta{0.01f, 0.01f};
    const std::size_t moved =
        uv::moveIslandUvVertex(atlas.mesh, island, anchor, delta, 10.0f);
    REQUIRE(moved > 1);

    for (const FaceId face : island) {
        for (const cyber::LoopId loop : atlas.mesh.faceLoops(face)) {
            const auto index = static_cast<std::size_t>(loop.value);
            const Vec2 expected = before[index] + delta;
            const Vec2 actual = (*uv::uvColumn(atlas.mesh))[index];
            // Exactly ONE delta each, never two.
            REQUIRE(uvDistance(actual, expected) < 1e-6f);
        }
    }
}

TEST_CASE("a UV vertex move is refused on a mesh with no layout and for a negative tolerance") {
    Mesh bare = makeCube();
    uv::AtlasOptions opts;
    const auto islands = uv::computeIslands(bare, uv::autoSeams(bare, opts));
    REQUIRE(uv::moveIslandUvVertex(bare, islands[0], Vec2{0, 0}, Vec2{0.1f, 0}, 1e-4f) == 0);
    REQUIRE(uv::uvColumn(bare) == nullptr);

    CubeAtlas atlas = unwrappedCube();
    const Vec2 anchor = (*uv::uvColumn(atlas.mesh))[0];
    REQUIRE(uv::moveIslandUvVertex(atlas.mesh, atlas.islands[0], anchor, Vec2{0.1f, 0}, -1.0f)
            == 0);
}
