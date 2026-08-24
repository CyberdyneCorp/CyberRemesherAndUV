#include <doctest.h>

#include <algorithm>
#include <cmath>
#include <cstddef>
#include <limits>
#include <string_view>
#include <vector>

#include "cyber/core/math.hpp"
#include "cyber/core/mesh.hpp"
#include "cyber/uv/atlas.hpp"
#include "cyber/uv/common.hpp"
#include "cyber/uv/distortion.hpp"
#include "cyber/uv/layout.hpp"
#include "cyber/uv/packing.hpp"
#include "cyber/uv/seams.hpp"
#include "cyber/uv/transforms.hpp"
#include "cyber/uv/unwrap.hpp"

using cyber::EdgeId;
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

// Flat n x n quad grid in the z=0 plane: one chart with many faces, so a single
// poisoned vertex still leaves plenty of well-formed triangles around it.
Mesh makeGrid(int n) {
    std::vector<Vec3> p;
    std::vector<std::vector<Index>> f;
    for (int y = 0; y <= n; ++y) {
        for (int x = 0; x <= n; ++x) {
            p.push_back({static_cast<float>(x), static_cast<float>(y), 0.0f});
        }
    }
    const Index stride = static_cast<Index>(n + 1);
    for (int y = 0; y < n; ++y) {
        for (int x = 0; x < n; ++x) {
            const Index b = static_cast<Index>(y) * stride + static_cast<Index>(x);
            f.push_back({b, b + 1, b + 1 + stride, b + stride});
        }
    }
    return Mesh::fromIndexed(p, f);
}

// A closed quad sphere. Every face carries a different normal, so chart growth
// fragments it and the distortion merge pass genuinely has to grind — unlike the
// hand-built cubes and strips above, where it converges in one round.
Mesh makeQuadSphere(int slices, int stacks) {
    std::vector<Vec3> p;
    std::vector<std::vector<Index>> f;
    for (int j = 0; j <= stacks; ++j) {
        const float v = cyber::kPi * static_cast<float>(j) / static_cast<float>(stacks);
        for (int i = 0; i < slices; ++i) {
            const float u = 2.0f * cyber::kPi * static_cast<float>(i) / static_cast<float>(slices);
            p.push_back({std::sin(v) * std::cos(u), std::cos(v), std::sin(v) * std::sin(u)});
        }
    }
    const auto id = [slices](int i, int j) {
        return static_cast<Index>(j * slices + (i % slices));
    };
    for (int j = 0; j < stacks; ++j) {
        for (int i = 0; i < slices; ++i) {
            f.push_back({id(i, j), id(i, j + 1), id(i + 1, j + 1), id(i + 1, j)});
        }
    }
    return Mesh::fromIndexed(p, f);
}

bool allUvFinite(const Mesh& mesh) {
    const std::vector<Vec2>* column = uv::uvColumn(mesh);
    if (column == nullptr) {
        return true;
    }
    return std::all_of(column->begin(), column->end(), [](Vec2 p) { return uv::isFinite(p); });
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

TEST_CASE("packing never reports a non-finite placement as a successful pack") {
    // Bounds2::valid() rejects a NaN box (comparisons against NaN are false)
    // but NOT an infinite one, so an infinite island used to reach the
    // strategies: the skyline path computed inf/inf = NaN and fed it to
    // `static_cast<int>(std::ceil(...))` (undefined behaviour), and the shelf
    // path emitted NaN placements alongside ok = true. Every real island must
    // still pack, and every placement must be finite.
    uv::Bounds2 good;
    good.expand({0.0f, 0.0f});
    good.expand({1.0f, 1.0f});
    uv::Bounds2 infinite;
    infinite.expand({0.0f, 0.0f});
    infinite.expand(
        {std::numeric_limits<float>::infinity(), std::numeric_limits<float>::infinity()});
    uv::Bounds2 notANumber;
    notANumber.expand({0.0f, 0.0f});
    notANumber.expand(
        {std::numeric_limits<float>::quiet_NaN(), std::numeric_limits<float>::quiet_NaN()});
    const std::vector<uv::Bounds2> boxes = {good, infinite, notANumber, good};

    for (const uv::PackStrategy strategy : {uv::PackStrategy::Shelf, uv::PackStrategy::Skyline}) {
        uv::PackParams params;
        params.strategy = strategy;
        const uv::PackResult packed = uv::packBoxes(boxes, params);
        REQUIRE(packed.ok);
        REQUIRE(packed.islands.size() == boxes.size());
        REQUIRE(std::isfinite(packed.scale));
        for (const uv::PackedIsland& island : packed.islands) {
            CHECK(std::isfinite(island.placed.mn.x));
            CHECK(std::isfinite(island.placed.mn.y));
            CHECK(std::isfinite(island.placed.mx.x));
            CHECK(std::isfinite(island.placed.mx.y));
        }
        // The two real islands keep a real extent instead of collapsing.
        CHECK(packed.islands[0].placed.size().x > 0.0f);
        CHECK(packed.islands[3].placed.size().x > 0.0f);
    }

    // A non-finite margin is the same class of poison and is neutralized too.
    uv::PackParams badMargin;
    badMargin.margin = std::numeric_limits<float>::infinity();
    const uv::PackResult packed = uv::packBoxes(std::vector<uv::Bounds2>{good, good}, badMargin);
    REQUIRE(packed.ok);
    CHECK(std::isfinite(packed.islands[0].placed.mx.x));
    CHECK(packed.islands[0].placed.size().x > 0.0f);
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

// The distortion merge drives a fixpoint over adjacent chart pairs and its
// accept predicate LSCM-unwraps the union of each candidate pair, so re-testing
// a pair whose charts have not changed since it was rejected costs a full trial
// unwrap for an answer already known. The four cases below pin the cost and the
// escape hatch of that pass, which is on by default.

TEST_CASE("the chart merge does not re-test pairs it already rejected") {
    Mesh mesh = makeQuadSphere(40, 20);
    int trials = 0;
    const cyber::CancelToken token;
    token.setPoll([&trials]() {
        ++trials;
        return false;
    });
    const uv::AtlasResult atlas = uv::unwrapAtlas(mesh, {}, nullptr, &token);
    REQUIRE(atlas.ok);

    // The token is polled once per merge trial (plus once after seaming), so
    // `trials` counts the trial unwraps the merge passes ran. A fixpoint that
    // forgets its rejections re-runs most of them on every round and needs 206
    // here; remembering them needs 112. The bound sits between the two with
    // room to spare on the remembering side.
    REQUIRE(trials < 160);

    // ...and it reaches exactly the partition the forgetful fixpoint reached:
    // skipping a re-test can only skip an answer that was already known.
    REQUIRE(atlas.chartCount == 8);
    REQUIRE(atlas.seamEdges == 242);
}

TEST_CASE("a cancel token that never fires changes nothing about the atlas") {
    Mesh plain = makeQuadSphere(24, 12);
    const uv::AtlasResult expected = uv::unwrapAtlas(plain);

    Mesh watched = makeQuadSphere(24, 12);
    const cyber::CancelToken token;
    token.setPoll([]() { return false; });
    float lastProgress = -1.0f;
    cyber::ProgressSink sink([&lastProgress](float p, std::string_view) {
        REQUIRE(p >= lastProgress);  // the sink is monotonic
        lastProgress = p;
    });
    const uv::AtlasResult observed = uv::unwrapAtlas(watched, {}, &sink, &token);

    REQUIRE(observed.ok);
    REQUIRE_FALSE(observed.cancelled);
    REQUIRE(observed.chartCount == expected.chartCount);
    REQUIRE(observed.seamEdges == expected.seamEdges);
    REQUIRE(observed.maxAngleDistortion == expected.maxAngleDistortion);
    REQUIRE(observed.packedArea == expected.packedArea);
    REQUIRE(lastProgress == doctest::Approx(1.0f));

    const std::vector<Vec2>* a = uv::uvColumn(plain);
    const std::vector<Vec2>* b = uv::uvColumn(watched);
    REQUIRE(a != nullptr);
    REQUIRE(b != nullptr);
    REQUIRE(a->size() == b->size());
    for (std::size_t i = 0; i < a->size(); ++i) {
        REQUIRE((*a)[i].x == (*b)[i].x);
        REQUIRE((*a)[i].y == (*b)[i].y);
    }
}

TEST_CASE("a cancelled unwrap leaves the mesh exactly as it was") {
    Mesh mesh = makeQuadSphere(24, 12);
    // Pre-existing UVs the caller would lose if a cancel wrote a partial atlas.
    std::vector<Vec2>& before = uv::ensureUvColumn(mesh);
    std::fill(before.begin(), before.end(), Vec2{0.25f, 0.75f});
    const std::vector<Vec2> snapshot = before;

    const cyber::CancelToken token;
    token.requestCancel();
    const uv::AtlasResult atlas = uv::unwrapAtlas(mesh, {}, nullptr, &token);

    REQUIRE(atlas.cancelled);
    REQUIRE_FALSE(atlas.ok);
    REQUIRE(atlas.chartCount == 0);
    const std::vector<Vec2>* after = uv::uvColumn(mesh);
    REQUIRE(after != nullptr);
    REQUIRE(*after == snapshot);
}

TEST_CASE("a cancel raised mid-merge aborts promptly instead of running to the end") {
    Mesh mesh = makeQuadSphere(40, 20);
    int polls = 0;
    const cyber::CancelToken token;
    token.setPoll([&polls]() { return ++polls > 5; });
    const uv::AtlasResult atlas = uv::unwrapAtlas(mesh, {}, nullptr, &token);

    REQUIRE(atlas.cancelled);
    // The token latches on the sixth poll and the pass stops there; running the
    // merge to its fixpoint would have taken over a hundred more trials.
    REQUIRE(polls == 6);
    // Nothing was written, so the mesh never grew a UV column.
    REQUIRE(uv::uvColumn(mesh) == nullptr);
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

TEST_CASE("packedArea is the fraction of the UV square geometry really covers") {
    // LSCM lays each cube face out as a 45-degree diamond, whose axis-aligned
    // bounding box is twice its area. Summing bounding boxes therefore reports
    // roughly double the coverage a texture painter would see.
    Mesh mesh = makeCube();
    uv::AtlasOptions opts;
    opts.maxChartDistortion = 0.0f;  // one chart per face -> diamonds
    opts.reorientCharts = false;     // keep them diamonds
    const uv::AtlasResult atlas = uv::unwrapAtlas(mesh, opts);
    REQUIRE(atlas.ok);

    // Ground truth: the summed |UV area| of every face, in the unit square.
    const std::vector<FaceId> faces = aliveFaces(mesh);
    const double covered = uv::islandUvArea(mesh, faces);

    REQUIRE(atlas.packedArea == doctest::Approx(static_cast<float>(covered)).epsilon(1e-3));
    // The bounding-box figure is reported separately and is materially larger.
    REQUIRE(atlas.packedBoxArea > atlas.packedArea * 1.4f);
    REQUIRE(atlas.packedArea > 0.0f);
    REQUIRE(atlas.packedArea <= 1.0f);
}

TEST_CASE("charts that never land are reported as dropped, not as charts") {
    // Two islands: a unit quad, and a quad whose four vertices are collinear.
    // The degenerate one has no surface, so neither LSCM nor the planar
    // fallback can give it area — it covers nothing in the packed atlas and
    // must not inflate chartCount.
    const std::vector<Vec3> p = {{0, 0, 0}, {1, 0, 0}, {1, 1, 0}, {0, 1, 0},
                                 {2, 0, 0}, {3, 0, 0}, {4, 0, 0}, {5, 0, 0}};
    const std::vector<std::vector<Index>> f = {{0, 1, 2, 3}, {4, 5, 6, 7}};
    Mesh mesh = Mesh::fromIndexed(p, f);

    const uv::AtlasResult atlas = uv::unwrapAtlas(mesh);
    REQUIRE(atlas.ok);
    REQUIRE(uv::computeIslands(mesh, uv::autoSeams(mesh)).size() == 2);

    // One chart lands, one is dropped; the two must reconcile with the islands.
    REQUIRE(atlas.chartCount == 1);
    REQUIRE(atlas.droppedCharts == 1);

    // ...and the surviving chart is exactly the one with UV area.
    int withArea = 0;
    for (const FaceId face : aliveFaces(mesh)) {
        const std::vector<FaceId> single{face};
        if (uv::islandUvArea(mesh, single) > 0.0) {
            ++withArea;
        }
    }
    REQUIRE(withArea == atlas.chartCount);
}

TEST_CASE("a non-finite vertex position is refused instead of unwrapping to non-finite UVs") {
    // Control: the same grid without the poisoned vertex unwraps and packs.
    Mesh clean = makeGrid(4);
    const uv::AtlasResult cleanAtlas = uv::unwrapAtlas(clean);
    REQUIRE(cleanAtlas.ok);
    REQUIRE(cleanAtlas.chartCount == 1);
    REQUIRE(allUvFinite(clean));

    // NaN fails every ordered comparison and inf survives them, so both slipped
    // past the degenerate-triangle and pin-length guards.
    const float poison[] = {std::numeric_limits<float>::infinity(),
                            std::numeric_limits<float>::quiet_NaN()};
    for (const float value : poison) {
        Mesh mesh = makeGrid(4);
        const VertexId bad{7};
        Vec3 p = mesh.position(bad);
        p.z = value;
        mesh.setPosition(bad, p);

        // The solve must not claim success on an island it cannot parameterize.
        REQUIRE_FALSE(uv::lscmUnwrap(mesh, aliveFaces(mesh)).ok);

        // ...and the atlas must report that failure rather than write the
        // non-finite values into the mesh's corners.
        const uv::AtlasResult atlas = uv::unwrapAtlas(mesh);
        REQUIRE_FALSE(atlas.ok);
        REQUIRE(allUvFinite(mesh));
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

// --- unwrap along caller-supplied seams --------------------------------------
// The manual UV workflow used to dead-end: a caller could mark seams (or commit
// a routed SeamPath) and then had no way to parameterize along them, because the
// only unwrap the bindings reach computes its own cuts. These pin the option
// that closes it.

TEST_CASE("a caller-supplied seam set defines the charts") {
    Mesh mesh = makeCube();
    // Cut the cube's top face free: the four edges of face 1 (4,5,6,7).
    uv::SeamSet seams;
    const std::vector<std::pair<Index, Index>> ring = {{4, 5}, {5, 6}, {6, 7}, {7, 4}};
    for (const auto& [a, b] : ring) {
        const EdgeId e = uv::faceEdge(mesh, VertexId{a}, VertexId{b});
        REQUIRE(e.valid());
        seams.mark(e);
    }
    REQUIRE(seams.size() == 4);

    uv::AtlasOptions opts;
    opts.seams = &seams;
    const uv::AtlasResult r = uv::unwrapAtlas(mesh, opts);

    REQUIRE(r.ok);
    // Exactly the split the seams describe: the lid, and the rest of the cube.
    CHECK(r.chartCount == 2);
    CHECK(r.seamEdges == 4);
    // Reported through the same structure the automatic atlas fills.
    CHECK(r.packedArea > 0.0f);
    CHECK(r.flippedCharts == 0);
}

TEST_CASE("an empty seam set unwraps the whole mesh as one chart") {
    Mesh mesh = makeCube();
    const uv::SeamSet none;
    uv::AtlasOptions opts;
    opts.seams = &none;

    const uv::AtlasResult r = uv::unwrapAtlas(mesh, opts);
    // The automatic path would have seamed this cube into several charts; an
    // explicit empty set means "do not cut", not "decide for me".
    CHECK(r.chartCount == 1);
    CHECK(r.seamEdges == 0);
}

TEST_CASE("chart growth and merge options are inert once seams are supplied") {
    Mesh mesh = makeCube();
    uv::SeamSet seams;
    for (const auto& [a, b] :
         std::vector<std::pair<Index, Index>>{{4, 5}, {5, 6}, {6, 7}, {7, 4}}) {
        seams.mark(uv::faceEdge(mesh, VertexId{a}, VertexId{b}));
    }

    uv::AtlasOptions tight;
    tight.seams = &seams;
    tight.maxChartAngleDeg = 1.0f;
    tight.mergeCharts = false;
    tight.maxChartDistortion = 0.0f;

    uv::AtlasOptions loose;
    loose.seams = &seams;
    loose.maxChartAngleDeg = 179.0f;
    loose.mergeCharts = true;
    loose.maxChartDistortion = 0.9f;

    Mesh a = mesh, b = mesh;
    const uv::AtlasResult ra = uv::unwrapAtlas(a, tight);
    const uv::AtlasResult rb = uv::unwrapAtlas(b, loose);

    // The seams decided the charts, so nothing that only steers seam CHOICE can move.
    CHECK(ra.chartCount == rb.chartCount);
    CHECK(ra.seamEdges == rb.seamEdges);
    CHECK(ra.packedArea == doctest::Approx(rb.packedArea));
}

TEST_CASE("supplying no seam set leaves the automatic atlas untouched") {
    Mesh a = makeCube(), b = makeCube();
    uv::AtlasOptions autoOpts;  // seams == nullptr
    uv::AtlasOptions explicitNull;
    explicitNull.seams = nullptr;

    const uv::AtlasResult ra = uv::unwrapAtlas(a, autoOpts);
    const uv::AtlasResult rb = uv::unwrapAtlas(b, explicitNull);
    CHECK(ra.chartCount == rb.chartCount);
    CHECK(ra.seamEdges == rb.seamEdges);
    CHECK(ra.maxAngleDistortion == doctest::Approx(rb.maxAngleDistortion));
}

TEST_CASE("sewing a seam rejoins the islands it separated") {
    Mesh mesh = makeCube();
    uv::SeamSet seams;
    std::vector<EdgeId> ring;
    for (const auto& [a, b] :
         std::vector<std::pair<Index, Index>>{{4, 5}, {5, 6}, {6, 7}, {7, 4}}) {
        const EdgeId e = uv::faceEdge(mesh, VertexId{a}, VertexId{b});
        seams.mark(e);
        ring.push_back(e);
    }
    REQUIRE(uv::computeIslands(mesh, seams).size() == 2);

    uv::AtlasOptions opts;
    opts.seams = &seams;
    REQUIRE(uv::unwrapAtlas(mesh, opts).ok);

    uv::stitchAlongSeams(mesh, seams, ring);

    CHECK(seams.size() == 0);
    CHECK(uv::computeIslands(mesh, seams).size() == 1);
}
