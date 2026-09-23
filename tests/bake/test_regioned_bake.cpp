#include <doctest.h>

#include <algorithm>
#include <cmath>
#include <cstddef>
#include <cstdint>
#include <string>
#include <string_view>
#include <vector>

#include "cyber/bake/bake.hpp"
#include "cyber/bake/field_evaluator.hpp"
#include "cyber/bake/map_catalog.hpp"
#include "cyber/core/io.hpp"
#include "cyber/core/mesh.hpp"
#include "cyber/core/progress.hpp"
#include "cyber/core/threading.hpp"

using cyber::CancelToken;
using cyber::FaceId;
using cyber::Index;
using cyber::LoopId;
using cyber::Mesh;
using cyber::ProgressSink;
using cyber::Vec2;
using cyber::Vec3;
namespace bake = cyber::bake;

// Regioned baking (surface-baking spec, "Regioned baking with a bounded working
// set").
//
// The contract is one sentence -- the assembled output equals the unregioned
// bake, texel for texel -- and most of what is asserted here is that sentence
// applied to what a naive tiled implementation gets wrong, none of which is
// visible inside any single region:
//
//   * a stage that reads a texel's NEIGHBOURHOOD (the padded band) reading an
//     incomplete one at a region edge;
//   * a quantity normalized over the WHOLE IMAGE -- the relative density mean,
//     the padded band's clamp range, a field curvature auto-range -- taken per
//     region, which is a different number in every region.
//
// Each produces output that is internally perfect in every region and steps at
// every seam, so "each region looks right" is worth nothing as a test: every
// comparison below is against the whole unregioned map.
namespace {

// ---- fixtures -------------------------------------------------------------

Mesh emptyWithUv() {
    Mesh mesh;
    mesh.cornerAttributes().create<Vec2>(cyber::io::kUvAttribute);
    return mesh;
}

// A height field over [x0, x1] x [y0, y1] sampled on an n x n grid of quads, its
// UVs mapping (x, y) linearly onto [u0, u1] x [v0, v1]. Wound so the face normal
// points +Z (or -Z with `down`).
struct Sheet {
    float x0, x1, y0, y1;
    float u0, u1, v0, v1;
    int n;
    bool down;
};

float lowHeight(float x, float y) { return 0.1f * x * y; }

float highHeight(float x, float y) {
    return lowHeight(x, y) + 0.04f * std::sin(7.0f * x) * std::cos(6.0f * y);
}

template <typename Height>
void addSheet(Mesh& mesh, const Sheet& s, Height height) {
    auto* uv = mesh.cornerAttributes().find<Vec2>(cyber::io::kUvAttribute);
    REQUIRE(uv != nullptr);
    const auto at = [](int i, int n, float a, float b) {
        return a + (b - a) * static_cast<float>(i) / static_cast<float>(n);
    };
    for (int j = 0; j < s.n; ++j) {
        for (int i = 0; i < s.n; ++i) {
            std::vector<Vec3> p;
            std::vector<Vec2> t;
            const int corners[4][2] = {{i, j}, {i + 1, j}, {i + 1, j + 1}, {i, j + 1}};
            for (const auto& c : corners) {
                const float x = at(c[0], s.n, s.x0, s.x1);
                const float y = at(c[1], s.n, s.y0, s.y1);
                p.push_back(Vec3{x, y, height(x, y)});
                t.push_back(Vec2{at(c[0], s.n, s.u0, s.u1), at(c[1], s.n, s.v0, s.v1)});
            }
            if (s.down) {
                std::reverse(p.begin(), p.end());
                std::reverse(t.begin(), t.end());
            }
            std::vector<cyber::VertexId> verts;
            for (const Vec3& q : p) {
                verts.push_back(mesh.addVertex(q));
            }
            const FaceId face = mesh.addFace(verts);
            std::size_t k = 0;
            for (const LoopId l : mesh.faceLoops(face)) {
                (*uv)[l.value] = t[k++];
            }
        }
    }
}

// The EditMesh: two islands of DIFFERENT UV density (the second is squashed in
// v), so a relative density map has two brightness levels, and a gently curved
// surface so the normal, position and curvature maps all vary down the image.
Mesh editMesh() {
    Mesh mesh = emptyWithUv();
    addSheet(mesh, Sheet{0, 1, 0, 1, 0.05f, 0.45f, 0.05f, 0.9f, 6, false}, lowHeight);
    addSheet(mesh, Sheet{1.3f, 2.3f, 0, 1, 0.55f, 0.95f, 0.3f, 0.8f, 6, false}, lowHeight);
    return mesh;
}

// The Target: a bumpy sheet over the EditMesh (so the cage ray lands and the
// hemisphere rays see relief), a downward-facing sheet under it (so thickness
// measures something), vertex colours and a material id per face.
Mesh target() {
    Mesh mesh = emptyWithUv();
    addSheet(mesh, Sheet{-0.3f, 2.6f, -0.3f, 1.3f, 0, 1, 0, 1, 40, false}, highHeight);
    addSheet(mesh, Sheet{-0.3f, 2.6f, -0.3f, 1.3f, 0, 1, 0, 1, 8, true},
             [](float, float) { return -0.2f; });
    auto& colors = mesh.vertexAttributes().create<Vec3>(cyber::io::kColorAttribute);
    colors.resize(mesh.vertexCapacity());
    for (Index v = 0; v < mesh.vertexCapacity(); ++v) {
        const Vec3 p = mesh.position(cyber::VertexId{v});
        colors[v] = Vec3{p.x / 2.6f, p.y, 0.5f + p.z};
    }
    auto& ids = mesh.faceAttributes().create<std::int32_t>("material_id");
    ids.resize(mesh.faceCapacity());
    for (Index f = 0; f < mesh.faceCapacity(); ++f) {
        ids[f] = static_cast<std::int32_t>(f % 3);
    }
    return mesh;
}

bake::BakeParams params(int size, int padding, std::size_t workingSet) {
    bake::BakeParams p;
    p.width = size;
    p.height = size;
    p.cageDistance = 0.2f;
    p.aoSamples = 16;
    p.aoRadius = 0.5f;
    p.paddingRadius = padding;
    p.maxWorkingSetTexels = workingSet;
    return p;
}

// ---- sinks ----------------------------------------------------------------

// Reassembles every image from its regions and remembers HOW the rows arrived:
// ascending, each exactly once, and never more than the plan said at once.
class Assembling final : public bake::RegionSink {
public:
    bool consume(const bake::RegionRows& rows) override {
        if (images.empty() || images.back().tile.number != rows.tile.number) {
            Tile tile;
            tile.tile = rows.tile;
            tile.image.width = rows.width;
            tile.image.height = rows.height;
            tile.image.channels = rows.channels;
            tile.image.pixels.assign(static_cast<std::size_t>(rows.width) *
                                         static_cast<std::size_t>(rows.height) *
                                         static_cast<std::size_t>(rows.channels),
                                     -1.0f);
            images.push_back(tile);
        }
        Tile& tile = images.back();
        ordered = ordered && rows.rowBegin == tile.nextRow;
        tile.nextRow = rows.rowBegin + rows.rowCount;
        ++bands;
        tallestBand = std::max(tallestBand, rows.rowCount);
        const std::size_t stride =
            static_cast<std::size_t>(rows.width) * static_cast<std::size_t>(rows.channels);
        std::copy(
            rows.pixels, rows.pixels + stride * static_cast<std::size_t>(rows.rowCount),
            tile.image.pixels.begin() +
                static_cast<std::ptrdiff_t>(static_cast<std::size_t>(rows.rowBegin) * stride));
        return true;
    }

    struct Tile {
        bake::UdimTile tile;
        bake::Image image;
        int nextRow = 0;
    };
    std::vector<Tile> images;
    bool ordered = true;
    int bands = 0;
    int tallestBand = 0;
};

// Counts rows and remembers nothing, for outputs that must not be held.
class Counting final : public bake::RegionSink {
public:
    bool consume(const bake::RegionRows& rows) override {
        ordered = ordered && rows.rowBegin == nextRow;
        nextRow = rows.rowBegin + rows.rowCount;
        rowsSeen += static_cast<std::size_t>(rows.rowCount);
        // The first and last row of every band finite: a synthesised or padded
        // row is still a map. (Every row would cost more than the bake.)
        const std::size_t stride =
            static_cast<std::size_t>(rows.width) * static_cast<std::size_t>(rows.channels);
        const float* last = rows.pixels + stride * static_cast<std::size_t>(rows.rowCount - 1);
        for (std::size_t i = 0; i < stride; ++i) {
            finite = finite && std::isfinite(rows.pixels[i]) && std::isfinite(last[i]);
        }
        return true;
    }
    std::size_t rowsSeen = 0;
    int nextRow = 0;
    bool ordered = true;
    bool finite = true;
};

void checkSameEncoding(const bake::BakeEncoding& a, const bake::BakeEncoding& b) {
    CHECK(a.basis == b.basis);
    CHECK(a.densityMean == b.densityMean);
    CHECK(a.boundsMin.x == b.boundsMin.x);
    CHECK(a.boundsMax.z == b.boundsMax.z);
    CHECK(a.idColors.size() == b.idColors.size());
    CHECK(a.idSource == b.idSource);
}

void checkSamePadding(const bake::BakePadding& a, const bake::BakePadding& b) {
    CHECK(a.radius == b.radius);
    CHECK(a.mode == b.mode);
    CHECK(a.texelsFilled == b.texelsFilled);
}

// Bakes `map` both ways and requires the regioned rows to equal the whole map.
void checkRegionedEqualsWhole(bake::BakeMap map, const bake::BakeParams& p,
                              std::size_t minRegions) {
    const Mesh low = editMesh();
    const Mesh high = target();
    const bake::BakeResult whole = bake::bake(low, high, map, p);
    REQUIRE_FALSE(whole.image.pixels.empty());

    Assembling sink;
    const bake::RegionedBakeResult regioned = bake::bakeRegions(low, high, map, p, false, sink);
    REQUIRE(regioned.refusal == bake::UdimRefusal::None);
    REQUIRE(regioned.failure == bake::RegionFailure::None);
    REQUIRE_FALSE(regioned.cancelled);
    CHECK(regioned.plan.regionCount >= minRegions);
    REQUIRE(sink.images.size() == 1);
    CHECK(sink.ordered);
    CHECK(sink.images.front().nextRow == p.height);
    CHECK(sink.images.front().image.pixels == whole.image.pixels);
    REQUIRE(regioned.tiles.size() == 1);
    checkSameEncoding(regioned.tiles.front().encoding, whole.encoding);
    checkSamePadding(regioned.tiles.front().padding, whole.padding);
    CHECK(regioned.tiles.front().texelsCovered == whole.texelsCovered);
}

// A working-set bound giving regions of `rows` rows for a `size`-wide map at
// `padding`.
std::size_t boundFor(int size, int padding, int rows) {
    bake::BakeParams p;
    p.paddingRadius = padding;
    const int halo = bake::regionHaloRows(p);
    return static_cast<std::size_t>(size) * static_cast<std::size_t>(rows + 2 * halo);
}

}  // namespace

// ---- the plan --------------------------------------------------------------

TEST_CASE("the halo is twice the padding radius and at least the derivative footprint") {
    bake::BakeParams p;
    p.paddingRadius = 8;
    CHECK(bake::regionHaloRows(p) == 16);
    p.paddingRadius = 0;
    CHECK(bake::regionHaloRows(p) == bake::kDerivativeFootprintTexels);
}

TEST_CASE("a working-set bound plans regions; zero plans one") {
    bake::BakeParams p = params(1024, 8, 0);
    bake::RegionPlan plan = bake::planRegions(p);
    CHECK(plan.regionCount == 1);
    CHECK(plan.regionRows == 1024);
    CHECK(plan.workingSetTexels == 1024u * 1024u);
    CHECK(plan.boundReached);

    p.maxWorkingSetTexels = 1024u * 96u;  // 96 rows: 64 of region, 2 x 16 of halo
    plan = bake::planRegions(p);
    CHECK(plan.regionRows == 64);
    CHECK(plan.haloRows == 16);
    CHECK(plan.regionCount == 16);
    CHECK(plan.workingSetTexels == 1024u * 96u);
    CHECK(plan.boundReached);
}

TEST_CASE("a bound below one row plus its halo is honoured as far as it goes and reported") {
    bake::BakeParams p = params(16384, 8, 1);
    const bake::RegionPlan plan = bake::planRegions(p);
    CHECK(plan.regionRows == 1);
    CHECK(plan.regionCount == 16384);
    CHECK(plan.workingSetTexels == 16384u * 33u);
    CHECK_FALSE(plan.boundReached);
}

// ---- identity with the unregioned bake -------------------------------------

TEST_CASE("a regioned bake equals the unregioned bake for every map type") {
    for (const bake::MapInfo& info : bake::mapCatalog()) {
        CAPTURE(std::string(info.name));
        // Several region heights, including a remainder region and one-row
        // regions under a bound below the floor.
        checkRegionedEqualsWhole(info.map, params(48, 4, boundFor(48, 4, 5)), 5);
        checkRegionedEqualsWhole(info.map, params(48, 8, boundFor(48, 8, 7)), 5);
        checkRegionedEqualsWhole(info.map, params(40, 2, 1), 40);
        checkRegionedEqualsWhole(info.map, params(48, 0, boundFor(48, 0, 3)), 10);
    }
}

TEST_CASE("a bound of zero runs one region and equals bake()") {
    checkRegionedEqualsWhole(bake::BakeMap::Normal, params(48, 8, 0), 1);
    checkRegionedEqualsWhole(bake::BakeMap::UvDensity, params(48, 8, 0), 1);
}

TEST_CASE("a gradient crossing a region boundary has no discontinuity") {
    // The object-space position map ramps smoothly down every island. The
    // regioned rows must equal the unregioned rows -- and, stated the way the
    // issue states it, the step across each region boundary must be the step
    // the whole map has there, not a jump the whole map lacks.
    const bake::BakeParams p = params(64, 8, boundFor(64, 8, 9));
    const Mesh low = editMesh();
    const Mesh high = target();
    const bake::BakeResult whole = bake::bake(low, high, bake::BakeMap::ObjectPosition, p);
    Assembling sink;
    const bake::RegionedBakeResult regioned =
        bake::bakeRegions(low, high, bake::BakeMap::ObjectPosition, p, false, sink);
    REQUIRE(regioned.tiles.size() == 1);
    REQUIRE(regioned.plan.regionCount > 3);
    const bake::Image& assembled = sink.images.front().image;
    int boundariesInsideTheRamp = 0;
    for (int b = regioned.plan.regionRows; b < p.height; b += regioned.plan.regionRows) {
        for (int x = 0; x < p.width; ++x) {
            const float before = assembled.at(x, b - 1, 1);
            const float after = assembled.at(x, b, 1);
            CHECK(after - before == whole.image.at(x, b, 1) - whole.image.at(x, b - 1, 1));
        }
        // Confirms the boundary really falls inside an island's ramp: the
        // position's y channel changes across it at the island's centre column.
        const int x = p.width / 4;
        if (whole.image.at(x, b, 1) != whole.image.at(x, b - 1, 1)) {
            ++boundariesInsideTheRamp;
        }
    }
    CHECK(boundariesInsideTheRamp >= 2);
}

TEST_CASE("a padded band crossing a region boundary equals the unregioned band") {
    // Island A's lower edge sits at v = 0.05, i.e. about two rows above the
    // bottom of a 48-row map; island B's edges sit mid-image. A region height of
    // 5 puts region boundaries inside both bands. The band is extrapolated
    // (AO: basis None) so it reads two texels out per ring and is clamped to
    // the WHOLE image's compounding range -- both of which a region-local pass
    // gets wrong.
    for (const bake::BakeMap map :
         {bake::BakeMap::AmbientOcclusion, bake::BakeMap::Displacement, bake::BakeMap::Normal,
          bake::BakeMap::ObjectPosition, bake::BakeMap::MaterialId}) {
        CAPTURE(static_cast<int>(map));
        checkRegionedEqualsWhole(map, params(48, 8, boundFor(48, 8, 5)), 9);
        checkRegionedEqualsWhole(map, params(48, 6, boundFor(48, 6, 2)), 20);
    }
}

TEST_CASE("a relative density map in regions divides by the whole map's mean") {
    bake::BakeParams p = params(64, 8, boundFor(64, 8, 6));
    p.densityNormalization = bake::DensityNormalization::Relative;
    const Mesh low = editMesh();
    const Mesh high = target();
    const bake::BakeResult whole = bake::bake(low, high, bake::BakeMap::UvDensity, p);
    Assembling sink;
    const bake::RegionedBakeResult regioned =
        bake::bakeRegions(low, high, bake::BakeMap::UvDensity, p, false, sink);
    REQUIRE(regioned.tiles.size() == 1);
    CHECK(regioned.plan.regionCount > 5);
    CHECK(regioned.tiles.front().encoding.densityMean == whole.encoding.densityMean);
    CHECK(whole.encoding.densityMean > 0.0f);
    CHECK(sink.images.front().image.pixels == whole.image.pixels);

    // The two islands really do sit at different relative levels: a per-region
    // mean would have normalized each of them towards 1.
    const bake::Image& image = sink.images.front().image;
    const float islandA = image.at(p.width / 4, p.height / 2, 0);
    const float islandB = image.at(3 * p.width / 4, p.height / 2, 0);
    CHECK(islandA > 0.0f);
    CHECK(islandB > 0.0f);
    CHECK(std::fabs(islandA - islandB) > 0.2f);
}

// ---- UDIM ------------------------------------------------------------------

TEST_CASE("a regioned UDIM set equals bakeUdim, relative mean over the whole set") {
    Mesh low = editMesh();
    // Move island B into tile 1002.
    auto* uv = low.cornerAttributes().find<Vec2>(cyber::io::kUvAttribute);
    for (Index f = 36; f < low.faceCapacity(); ++f) {
        for (const LoopId l : low.faceLoops(FaceId{f})) {
            (*uv)[l.value].x += 1.0f;
        }
    }
    const Mesh high = target();
    for (const bake::BakeMap map : {bake::BakeMap::UvDensity, bake::BakeMap::AmbientOcclusion,
                                    bake::BakeMap::ObjectPosition}) {
        CAPTURE(static_cast<int>(map));
        bake::BakeParams p = params(32, 4, boundFor(32, 4, 4));
        p.densityNormalization = bake::DensityNormalization::Relative;
        const bake::UdimBakeResult whole = bake::bakeUdim(low, high, map, p);
        REQUIRE(whole.tiles.size() == 2);
        Assembling sink;
        const bake::RegionedBakeResult regioned = bake::bakeRegions(low, high, map, p, true, sink);
        REQUIRE(regioned.tiles.size() == 2);
        REQUIRE(sink.images.size() == 2);
        for (std::size_t t = 0; t < 2; ++t) {
            CHECK(sink.images[t].tile.number == whole.tiles[t].tile.number);
            CHECK(sink.images[t].image.pixels == whole.tiles[t].result.image.pixels);
            checkSameEncoding(regioned.tiles[t].encoding, whole.tiles[t].result.encoding);
            checkSamePadding(regioned.tiles[t].padding, whole.tiles[t].result.padding);
        }
    }
}

TEST_CASE("a regioned UDIM set of one-region tiles still spills rather than hold the set") {
    Mesh low = editMesh();
    auto* uv = low.cornerAttributes().find<Vec2>(cyber::io::kUvAttribute);
    for (Index f = 36; f < low.faceCapacity(); ++f) {
        for (const LoopId l : low.faceLoops(FaceId{f})) {
            (*uv)[l.value].x += 1.0f;
        }
    }
    const Mesh high = target();
    bake::BakeParams p = params(32, 4, 32u * 32u);  // one tile fits; two do not
    Assembling sink;
    const bake::RegionedBakeResult regioned =
        bake::bakeRegions(low, high, bake::BakeMap::Normal, p, true, sink);
    REQUIRE(regioned.tiles.size() == 2);
    CHECK(regioned.plan.regionCount == 1);
    CHECK(regioned.peakTexelsInFlight <= 32u * 32u);
    const bake::UdimBakeResult whole = bake::bakeUdim(low, high, bake::BakeMap::Normal, p);
    CHECK(sink.images[1].image.pixels == whole.tiles[1].result.image.pixels);
}

// ---- the field path --------------------------------------------------------

namespace {

// A bumpy height field as a distance-like function: close enough to Lipschitz-1
// for the tracer, and curved, so an auto-ranged curvature map varies.
class BumpField final : public bake::FieldEvaluator {
public:
    float distance(Vec3 p) const override { return (p.z - highHeight(p.x, p.y)) * 0.8f; }
    Vec3 gradient(Vec3 p) const override {
        const float h = 1e-3f;
        return Vec3{-(highHeight(p.x + h, p.y) - highHeight(p.x - h, p.y)) / (2 * h),
                    -(highHeight(p.x, p.y + h) - highHeight(p.x, p.y - h)) / (2 * h), 1.0f};
    }
    float openness(Vec3 p, Vec3, float) const override {
        return std::clamp(0.5f + p.z, 0.0f, 1.0f);
    }
};

}  // namespace

TEST_CASE("a field-sampled auto-ranged curvature map takes its range over the whole image") {
    // The auto range is a percentile over every sampled texel of the image. A
    // per-region percentile saturates each region differently; the assembled
    // map must equal the whole one.
    const BumpField field;
    const Mesh low = editMesh();
    const Mesh noTarget;
    for (const bake::BakeMap map : {bake::BakeMap::Curvature, bake::BakeMap::Cavity,
                                    bake::BakeMap::Normal, bake::BakeMap::AmbientOcclusion}) {
        CAPTURE(static_cast<int>(map));
        bake::BakeParams p = params(48, 4, boundFor(48, 4, 5));
        p.field = &field;
        const bake::BakeResult whole = bake::bake(low, noTarget, map, p);
        REQUIRE_FALSE(whole.image.pixels.empty());
        Assembling sink;
        const bake::RegionedBakeResult regioned =
            bake::bakeRegions(low, noTarget, map, p, false, sink);
        REQUIRE(regioned.tiles.size() == 1);
        CHECK(regioned.plan.regionCount > 5);
        CHECK(sink.images.front().image.pixels == whole.image.pixels);
        CHECK(regioned.tiles.front().fieldUndefinedSamples == whole.fieldUndefinedSamples);
    }
}

// ---- the working set ---------------------------------------------------------

TEST_CASE("the working set is bounded while the output is not") {
    const Mesh low = editMesh();
    const Mesh high = target();
    const std::size_t bound = boundFor(256, 8, 8);  // 48 rows of 256 in flight
    const bake::BakeParams p = params(256, 8, bound);
    for (const bake::BakeMap map :
         {bake::BakeMap::Normal, bake::BakeMap::AmbientOcclusion, bake::BakeMap::UvDensity}) {
        CAPTURE(static_cast<int>(map));
        Assembling sink;
        const bake::RegionedBakeResult regioned = bake::bakeRegions(low, high, map, p, false, sink);
        REQUIRE(regioned.tiles.size() == 1);
        CHECK(regioned.plan.boundReached);
        CHECK(regioned.plan.workingSetTexels <= bound);
        // Measured, not planned: no buffer of output texels was larger.
        CHECK(regioned.peakTexelsInFlight <= bound);
        CHECK(regioned.peakTexelsInFlight * 5 < 256u * 256u);
        CHECK(sink.ordered);
        CHECK(sink.images.front().nextRow == 256);
        CHECK(sink.tallestBand == regioned.plan.regionRows);
    }
}

TEST_CASE("8192 and 16384 outputs of every map type are produced under a small working set") {
    // A chart of a few dozen texels, placed so that it straddles region
    // boundaries, at 8192 and 16384 square. The output is 64 M and 256 M
    // texels; the working set is a 64-row window. Nothing here holds the output:
    // the sink counts rows and checks they are finite, and the peak buffer
    // is measured.
    Mesh low = emptyWithUv();
    const float c = 0.5f;
    const float e = 0.002f;
    addSheet(low, Sheet{0, 1, 0, 1, c - e, c + e, c - e, c + e, 2, false}, lowHeight);
    const Mesh high = target();
    for (const int size : {8192, 16384}) {
        const std::size_t bound = static_cast<std::size_t>(size) * 256u;
        for (const bake::MapInfo& info : bake::mapCatalog()) {
            CAPTURE(size);
            CAPTURE(std::string(info.name));
            bake::BakeParams p = params(size, 8, bound);
            p.aoSamples = 4;
            p.maxPixels = static_cast<std::size_t>(size) * static_cast<std::size_t>(size);
            Counting sink;
            const bake::RegionedBakeResult regioned =
                bake::bakeRegions(low, high, info.map, p, false, sink);
            REQUIRE(regioned.refusal == bake::UdimRefusal::None);
            REQUIRE(regioned.failure == bake::RegionFailure::None);
            REQUIRE(regioned.tiles.size() == 1);
            CHECK(regioned.tiles.front().texelsCovered > 0);
            CHECK(sink.rowsSeen == static_cast<std::size_t>(size));
            CHECK(sink.ordered);
            CHECK(sink.finite);
            CHECK(regioned.peakTexelsInFlight <= bound);
        }
    }
}

// ---- cancellation and progress ---------------------------------------------

TEST_CASE("a cancelled regioned bake stops inside the first region and emits nothing") {
    // Two workers, so each of the hemisphere path's chunks holds more than the
    // 2048-texel poll stride and the stated latency is observable; the serial
    // rasterized path polls on the same stride.
    struct WorkerCap {
        WorkerCap() { cyber::setMaxWorkerThreads(2); }
        ~WorkerCap() { cyber::setMaxWorkerThreads(0); }
        WorkerCap(const WorkerCap&) = delete;
        WorkerCap& operator=(const WorkerCap&) = delete;
    } cap;
    const Mesh low = editMesh();
    const Mesh high = target();
    const bake::BakeParams p = params(256, 4, boundFor(256, 4, 128));  // two regions
    REQUIRE(bake::planRegions(p).regionCount == 2);
    for (const bake::BakeMap map : {bake::BakeMap::Normal, bake::BakeMap::AmbientOcclusion}) {
        CAPTURE(static_cast<int>(map));
        CancelToken cancel;
        float lastSeen = 0.0f;
        ProgressSink progress([&](float value, std::string_view) {
            lastSeen = value;
            if (value > 0.0f) {
                cancel.requestCancel();  // the first report inside region one
            }
        });
        Counting sink;
        const bake::RegionedBakeResult regioned =
            bake::bakeRegions(low, high, map, p, false, sink, &progress, &cancel);
        CHECK(regioned.cancelled);
        CHECK(regioned.tiles.empty());
        CHECK(sink.rowsSeen == 0);
        // Region one's shading owns [0, 0.45] of the bar; the bake never
        // finished it.
        CHECK(lastSeen < 0.45f);
    }
}

TEST_CASE("regioned progress is finer than one report per region and monotone") {
    const Mesh low = editMesh();
    const Mesh high = target();
    const bake::BakeParams p = params(96, 4, boundFor(96, 4, 24));  // four regions
    for (const bake::BakeMap map : {bake::BakeMap::Normal, bake::BakeMap::AmbientOcclusion}) {
        CAPTURE(static_cast<int>(map));
        std::vector<float> seen;
        ProgressSink progress([&seen](float value, std::string_view) { seen.push_back(value); });
        Counting sink;
        const bake::RegionedBakeResult regioned =
            bake::bakeRegions(low, high, map, p, false, sink, &progress);
        REQUIRE(regioned.tiles.size() == 1);
        REQUIRE(regioned.plan.regionCount == 4);
        CHECK(std::is_sorted(seen.begin(), seen.end()));
        CHECK(seen.back() == 1.0f);
        // Region one's shading slice alone carries several distinct values.
        std::vector<float> inFirst;
        for (const float v : seen) {
            if (v > 0.0f && v < 0.9f / 4.0f && (inFirst.empty() || inFirst.back() != v)) {
                inFirst.push_back(v);
            }
        }
        CHECK(inFirst.size() >= 3);
    }
}

// ---- refusals and failures ---------------------------------------------------

TEST_CASE("the texel ceiling bounds the output of a regioned bake, the bound never refuses") {
    const Mesh low = editMesh();
    const Mesh high = target();
    bake::BakeParams p = params(64, 4, 1);
    p.maxPixels = 64u * 63u;
    Counting sink;
    bake::RegionedBakeResult regioned =
        bake::bakeRegions(low, high, bake::BakeMap::Normal, p, false, sink);
    CHECK(regioned.refusal == bake::UdimRefusal::PerTileCeiling);
    CHECK(sink.rowsSeen == 0);

    p.maxPixels = 64u * 64u;  // the output fits; the working set of 1 texel does not
    regioned = bake::bakeRegions(low, high, bake::BakeMap::Normal, p, false, sink);
    CHECK(regioned.refusal == bake::UdimRefusal::None);
    CHECK_FALSE(regioned.plan.boundReached);
    CHECK(sink.rowsSeen == 64u);
}

TEST_CASE("a regioned bake refuses exactly what bake() refuses") {
    const Mesh low = editMesh();
    const Mesh high = target();
    bake::BakeParams p = params(32, -1, 32u * 20u);
    Counting sink;
    CHECK(bake::bakeRegions(low, high, bake::BakeMap::Normal, p, false, sink).refusal ==
          bake::UdimRefusal::Parameters);
    p.paddingRadius = 2;
    p.aoSamples = 0;
    CHECK(bake::bakeRegions(low, high, bake::BakeMap::AmbientOcclusion, p, false, sink).refusal ==
          bake::UdimRefusal::Parameters);
    CHECK(bake::bakeRegions(low, high, bake::BakeMap::Normal, p, false, sink).refusal ==
          bake::UdimRefusal::None);
    Mesh noUv;
    noUv.addVertex(Vec3{});
    CHECK(bake::bakeRegions(noUv, high, bake::BakeMap::Normal, p, false, sink).refusal ==
          bake::UdimRefusal::Parameters);
    CHECK(bake::bakeRegions(noUv, high, bake::BakeMap::Normal, p, true, sink).refusal ==
          bake::UdimRefusal::NoOccupiedTiles);
}

TEST_CASE("a sink that refuses rows abandons the bake") {
    class Refusing final : public bake::RegionSink {
    public:
        bool consume(const bake::RegionRows&) override { return ++calls < 2; }
        int calls = 0;
    };
    const Mesh low = editMesh();
    const Mesh high = target();
    Refusing sink;
    const bake::RegionedBakeResult regioned = bake::bakeRegions(
        low, high, bake::BakeMap::Normal, params(48, 4, boundFor(48, 4, 5)), false, sink);
    CHECK(regioned.failure == bake::RegionFailure::Sink);
    CHECK(regioned.tiles.empty());
    CHECK(sink.calls == 2);
}

TEST_CASE("an unusable scratch directory is a stated failure, not a partial map") {
    const Mesh low = editMesh();
    const Mesh high = target();
    Counting sink;
    const bake::RegionedBakeResult regioned =
        bake::bakeRegions(low, high, bake::BakeMap::Normal, params(48, 4, boundFor(48, 4, 5)),
                          false, sink, nullptr, nullptr, "/nonexistent-cyber-scratch-dir/x");
    CHECK(regioned.failure == bake::RegionFailure::Scratch);
    CHECK_FALSE(regioned.failureMessage.empty());
    CHECK(sink.rowsSeen == 0);
}
