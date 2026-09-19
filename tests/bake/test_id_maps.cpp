#include <doctest.h>

#include <algorithm>
#include <array>
#include <cmath>
#include <cstdint>
#include <set>
#include <string>
#include <string_view>
#include <utility>
#include <vector>

#include "cyber/bake/bake.hpp"
#include "cyber/core/mesh.hpp"
#include "cyber/core/progress.hpp"

using cyber::CancelToken;
using cyber::FaceId;
using cyber::Index;
using cyber::LoopId;
using cyber::Mesh;
using cyber::Vec2;
using cyber::Vec3;
namespace bake = cyber::bake;

// Colour-ID maps (surface-baking spec: "Material ID and object ID maps"). The
// whole value of these two maps is an EXACT comparison at zero tolerance, so
// every case here compares 8-bit bytes rather than floats with a tolerance --
// a test that allowed slack would pass on a map an artist cannot select with.
namespace {

// The EditMesh: one quad in the z = 0 plane spanning x in [x0, x1], whose UVs
// cover the WHOLE layout however narrow that span is. With the default span a
// texel's column says which half of the Target it speaks for; a narrower one
// puts the layout over part of the Target only.
Mesh lowPlaneOver(float x0, float x1) {
    const std::vector<Vec3> p = {{x0, 0, 0}, {x1, 0, 0}, {x1, 1, 0}, {x0, 1, 0}};
    const std::vector<std::vector<Index>> f = {{0, 1, 2, 3}};
    Mesh mesh = Mesh::fromIndexed(p, f);
    auto& uv = mesh.cornerAttributes().create<Vec2>("uv");
    for (Index fi = 0; fi < mesh.faceCapacity(); ++fi) {
        if (!mesh.isAlive(FaceId{fi})) {
            continue;
        }
        for (const LoopId l : mesh.faceLoops(FaceId{fi})) {
            const Vec3 pos = mesh.position(mesh.loopVertex(l));
            uv[l.value] = {(pos.x - x0) / (x1 - x0), pos.y};
        }
    }
    return mesh;
}

Mesh lowPlane() { return lowPlaneOver(0.0f, 1.0f); }

// The same quad, but with its UVs shrunk into the lower-left quarter of the
// layout: the texels outside it are never rasterised at all, so they keep
// whatever the pre-fill wrote. Every other case here covers the whole layout
// and therefore never exercises that path.
Mesh lowPlaneInQuarterLayout() {
    Mesh mesh = lowPlane();
    std::vector<Vec2>* uv = mesh.cornerAttributes().find<Vec2>("uv");
    for (Vec2& coord : *uv) {
        coord = {coord.x * 0.5f, coord.y * 0.5f};
    }
    return mesh;
}

// The Target: two quads at z = 0, LEFT over u < 0.5 and RIGHT over u > 0.5,
// built from separate vertices so they share no edge and read as two
// face-connected components. Both overhang the EditMesh so no cage ray at the
// border misses for want of surface.
Mesh splitTarget() {
    const std::vector<Vec3> p = {
        {-0.1f, -0.1f, 0}, {0.5f, -0.1f, 0}, {0.5f, 1.1f, 0}, {-0.1f, 1.1f, 0},
        {0.5f, -0.1f, 0},  {1.1f, -0.1f, 0}, {1.1f, 1.1f, 0}, {0.5f, 1.1f, 0},
    };
    const std::vector<std::vector<Index>> f = {{0, 1, 2, 3}, {4, 5, 6, 7}};
    return Mesh::fromIndexed(p, f);
}

// A Target whose faces carry `values` in the named face-domain column.
Mesh targetWithColumn(const char* column, const std::vector<std::int32_t>& values) {
    Mesh mesh = splitTarget();
    auto& ids = mesh.faceAttributes().create<std::int32_t>(column);
    for (std::size_t i = 0; i < values.size(); ++i) {
        ids[i] = values[i];
    }
    return mesh;
}

bake::BakeParams params64() {
    bake::BakeParams p;
    p.width = 64;
    p.height = 64;
    p.cageDistance = 0.05f;
    return p;
}

using Rgb = std::array<int, 3>;

// A texel as the 8-BIT triple that reaches the file: exactly what the PNG
// writer stores (clamp to [0,1], then lround(v * 255)), so a byte comparison
// here is a comparison of what a consumer will actually pick.
Rgb byteAt(const bake::Image& img, int px, int py) {
    Rgb out{};
    for (int c = 0; c < 3; ++c) {
        const float v = std::clamp(img.at(px, py, c), 0.0f, 1.0f);
        out[static_cast<std::size_t>(c)] = static_cast<int>(std::lround(v * 255.0f));
    }
    return out;
}

Rgb expected(std::int32_t id) {
    const std::array<std::uint8_t, 3> c = bake::idColor(id);
    return Rgb{c[0], c[1], c[2]};
}

constexpr Rgb kNoId{0, 0, 0};

// The float the image actually holds must be EXACTLY `byte / 255`, not merely
// close enough to round back to it. The 8-bit writer would hide a drift of up
// to half a step; EXR and any host reading the float buffer would not, and a
// consumer comparing at zero tolerance there would lose every texel.
bool floatsAreExact(const bake::Image& img, int px, int py, std::int32_t id) {
    const std::array<std::uint8_t, 3> c = bake::idColor(id);
    for (int channel = 0; channel < 3; ++channel) {
        const float want = static_cast<float>(c[static_cast<std::size_t>(channel)]) / 255.0f;
        if (img.at(px, py, channel) != want) {
            return false;
        }
    }
    return true;
}

// Every distinct 8-bit colour in the image, ordered. std::set for the TEST's
// convenience only: nothing the bake produces depends on it.
std::set<Rgb> distinctColors(const bake::Image& img) {
    std::set<Rgb> colors;
    for (int y = 0; y < img.height; ++y) {
        for (int x = 0; x < img.width; ++x) {
            colors.insert(byteAt(img, x, y));
        }
    }
    return colors;
}

}  // namespace

// ---- the colour rule ------------------------------------------------------

TEST_CASE("an id's colour is a pinned, integer-only function of the id alone") {
    // Pinned values, not recomputed from the implementation: they are the
    // cross-platform claim. The function is pure integer arithmetic -- no
    // transcendental, no container order, no counter -- so every platform this
    // suite runs on must produce exactly these bytes. A change here is a
    // deliberate break of every saved selection an artist already has.
    CHECK(expected(0) == Rgb{125, 235, 253});
    CHECK(expected(1) == Rgb{145, 166, 249});
    CHECK(expected(2) == Rgb{218, 122, 183});
    CHECK(expected(7) == Rgb{164, 196, 79});
    CHECK(expected(42) == Rgb{64, 104, 231});
    CHECK(expected(-1) == Rgb{184, 234, 232});
}

TEST_CASE("no assigned colour can be the reserved 'no id' black") {
    // The reservation is what lets an uncovered texel mean "nothing here"
    // unambiguously, so it has to be unreachable rather than merely unlikely.
    for (std::int32_t id = -2048; id <= 2048; ++id) {
        const Rgb c = expected(id);
        REQUIRE(c != kNoId);
        for (const int channel : c) {
            REQUIRE(channel >= 64);
            REQUIRE(channel <= 255);
        }
    }
}

TEST_CASE("an assigned colour survives the 8-bit quantisation the writer applies") {
    // The image holds floats; the PNG writer clamps and rounds them. A colour
    // off the 8-bit lattice would round to a DIFFERENT byte than the reported
    // table names, and every exact selection would then miss.
    for (std::int32_t id = 0; id < 512; ++id) {
        const std::array<std::uint8_t, 3> c = bake::idColor(id);
        for (const std::uint8_t channel : c) {
            const float stored = static_cast<float>(channel) / 255.0f;
            REQUIRE(std::lround(std::clamp(stored, 0.0f, 1.0f) * 255.0f) ==
                    static_cast<long>(channel));
        }
    }
}

TEST_CASE("an id's colour does not depend on which other ids exist") {
    // The property a palette assigned in traversal order cannot have: inserting
    // or removing an id must not repaint the rest of the model.
    const Mesh low = lowPlane();
    const bake::BakeParams p = params64();

    const Mesh two = targetWithColumn("material_id", {7, 42});
    const bake::BakeResult pair = bake::bake(low, two, bake::BakeMap::MaterialId, p);
    REQUIRE(!pair.image.pixels.empty());

    // The same id 42, now sitting beside a different neighbour and no longer
    // the second face the traversal reaches.
    const Mesh swapped = targetWithColumn("material_id", {42, 999});
    const bake::BakeResult moved = bake::bake(low, swapped, bake::BakeMap::MaterialId, p);
    REQUIRE(!moved.image.pixels.empty());

    CHECK(byteAt(pair.image, 48, 32) == expected(42));
    CHECK(byteAt(moved.image, 16, 32) == expected(42));
}

// ---- material ID ----------------------------------------------------------

TEST_CASE("a material ID map paints one flat colour per material") {
    const Mesh low = lowPlane();
    const Mesh high = targetWithColumn("material_id", {7, 42});
    const bake::BakeResult result = bake::bake(low, high, bake::BakeMap::MaterialId, params64());
    REQUIRE(!result.image.pixels.empty());
    REQUIRE(result.image.channels == 3);

    CHECK(byteAt(result.image, 16, 32) == expected(7));
    CHECK(byteAt(result.image, 48, 32) == expected(42));
    CHECK(floatsAreExact(result.image, 16, 32, 7));
    CHECK(floatsAreExact(result.image, 48, 32, 42));
    // Exactly two colours over the whole layout: no third value means no
    // interpolation, no anti-aliasing and no uncovered texel anywhere.
    const std::set<Rgb> colors = distinctColors(result.image);
    CHECK(colors.size() == 2);
    CHECK(colors.count(expected(7)) == 1);
    CHECK(colors.count(expected(42)) == 1);
}

TEST_CASE("a boundary texel is one id's colour or the other, never a blend") {
    const Mesh low = lowPlane();
    const Mesh high = targetWithColumn("material_id", {7, 42});
    const bake::BakeResult result = bake::bake(low, high, bake::BakeMap::MaterialId, params64());
    REQUIRE(!result.image.pixels.empty());

    // The two columns either side of u = 0.5, where an anti-aliased map would
    // put its average and an exact selection would lose every texel.
    for (int y = 0; y < result.image.height; ++y) {
        for (const int x : {31, 32}) {
            const Rgb c = byteAt(result.image, x, y);
            REQUIRE((c == expected(7) || c == expected(42)));
        }
    }
}

TEST_CASE("a Target with no material column reports that, rather than guessing") {
    const Mesh low = lowPlane();
    const bake::BakeResult result =
        bake::bake(low, splitTarget(), bake::BakeMap::MaterialId, params64());
    REQUIRE(!result.image.pixels.empty());
    // A connected component is an object, not a material, so there is no
    // fallback to make here: every face reads 0 and the report says so.
    CHECK(result.encoding.idSource == "none");
    REQUIRE(result.encoding.idColors.size() == 1);
    CHECK(result.encoding.idColors[0].id == 0);
    CHECK(distinctColors(result.image) == std::set<Rgb>{expected(0)});
}

// ---- object ID ------------------------------------------------------------

TEST_CASE("a merged multi-part Target separates through the component fallback") {
    const Mesh low = lowPlane();
    // No column at all: the two parts are known only by being disconnected,
    // which is the state every asset loaded through this tree is in.
    const bake::BakeResult result =
        bake::bake(low, splitTarget(), bake::BakeMap::ObjectId, params64());
    REQUIRE(!result.image.pixels.empty());

    CHECK(result.encoding.idSource == "component");
    CHECK(byteAt(result.image, 16, 32) == expected(0));
    CHECK(byteAt(result.image, 48, 32) == expected(1));
    CHECK(distinctColors(result.image).size() == 2);
}

TEST_CASE("a declared object column beats the component fallback") {
    const Mesh low = lowPlane();
    // One id over two disconnected parts. The declared column is the authority,
    // so the parts must NOT be split back apart by the fallback.
    const bake::BakeResult result =
        bake::bake(low, targetWithColumn("object_id", {5, 5}), bake::BakeMap::ObjectId, params64());
    REQUIRE(!result.image.pixels.empty());

    CHECK(result.encoding.idSource == "object_id");
    CHECK(distinctColors(result.image) == std::set<Rgb>{expected(5)});
    REQUIRE(result.encoding.idColors.size() == 1);
    CHECK(result.encoding.idColors[0].id == 5);
}

TEST_CASE("object_id wins when both columns are present") {
    // Precedence has to be pinned with BOTH columns on one Target: a Target
    // carrying only one of them passes whichever order the resolution happens
    // to use, so it proves nothing about the order.
    const Mesh low = lowPlane();
    Mesh high = targetWithColumn("object_id", {3, 8});
    auto& groups = high.faceAttributes().create<std::int32_t>("group_id");
    groups[0] = 100;
    groups[1] = 200;

    const bake::BakeResult result = bake::bake(low, high, bake::BakeMap::ObjectId, params64());
    REQUIRE(!result.image.pixels.empty());
    CHECK(result.encoding.idSource == "object_id");
    CHECK(byteAt(result.image, 16, 32) == expected(3));
    CHECK(byteAt(result.image, 48, 32) == expected(8));
}

TEST_CASE("group_id answers the object map when object_id is absent") {
    const Mesh low = lowPlane();
    const bake::BakeResult result =
        bake::bake(low, targetWithColumn("group_id", {3, 8}), bake::BakeMap::ObjectId, params64());
    REQUIRE(!result.image.pixels.empty());

    CHECK(result.encoding.idSource == "group_id");
    CHECK(byteAt(result.image, 16, 32) == expected(3));
    CHECK(byteAt(result.image, 48, 32) == expected(8));
}

// ---- the reported table ---------------------------------------------------

TEST_CASE("the reported table resolves every covered texel back to one id") {
    const Mesh low = lowPlane();
    const Mesh high = targetWithColumn("material_id", {42, 7});
    const bake::BakeResult result = bake::bake(low, high, bake::BakeMap::MaterialId, params64());
    REQUIRE(!result.image.pixels.empty());

    CHECK(result.encoding.basis == bake::EncodingBasis::IdColor);
    CHECK(result.encoding.idSource == "material_id");
    // Ascending by id -- a stable ordered key, not the order the faces carry
    // them in (the Target above declares 42 first).
    REQUIRE(result.encoding.idColors.size() == 2);
    CHECK(result.encoding.idColors[0].id == 7);
    CHECK(result.encoding.idColors[1].id == 42);

    // The consumer's job, done exactly: pick a colour, find its one row.
    for (int y = 0; y < result.image.height; y += 7) {
        for (int x = 0; x < result.image.width; x += 7) {
            const Rgb picked = byteAt(result.image, x, y);
            int matches = 0;
            std::int32_t resolved = 0;
            for (const bake::IdColorEntry& entry : result.encoding.idColors) {
                if (Rgb{entry.color[0], entry.color[1], entry.color[2]} == picked) {
                    ++matches;
                    resolved = entry.id;
                }
            }
            REQUIRE(matches == 1);
            // u < 0.5 is the first face, which this Target gave id 42.
            REQUIRE(resolved == (x < 32 ? 42 : 7));
        }
    }
}

TEST_CASE("the table lists every Target id, including ones the layout hides") {
    // The EditMesh sits over the left-hand material only, so the right-hand one
    // never reaches a texel. A consumer still needs its row: which ids the
    // layout happens to show is a property of the UVs, not of the assignment.
    const Mesh low = lowPlaneOver(0.0f, 0.4f);
    const Mesh high = targetWithColumn("material_id", {7, 42});
    const bake::BakeResult result = bake::bake(low, high, bake::BakeMap::MaterialId, params64());
    REQUIRE(!result.image.pixels.empty());

    REQUIRE(result.encoding.idColors.size() == 2);
    CHECK(result.encoding.idColors[0].id == 7);
    CHECK(result.encoding.idColors[1].id == 42);
    // 42 is on the Target but nowhere in the image: the EditMesh never reaches
    // it, yet a consumer still needs its row to resolve a colour picked from a
    // sibling map baked over the whole asset.
    CHECK(distinctColors(result.image) == std::set<Rgb>{expected(7)});
}

TEST_CASE("only an id map reports a table") {
    const Mesh low = lowPlane();
    const Mesh high = targetWithColumn("material_id", {7, 42});
    const bake::BakeResult normal = bake::bake(low, high, bake::BakeMap::Normal, params64());
    REQUIRE(!normal.image.pixels.empty());
    CHECK(normal.encoding.basis == bake::EncodingBasis::TangentNormal);
    CHECK(normal.encoding.idSource.empty());
    CHECK(normal.encoding.idColors.empty());
}

// ---- determinism ----------------------------------------------------------

TEST_CASE("the same id bake twice is identical, texel for texel and row for row") {
    const Mesh low = lowPlane();
    const Mesh high = targetWithColumn("object_id", {11, -4});
    const bake::BakeResult first = bake::bake(low, high, bake::BakeMap::ObjectId, params64());
    const bake::BakeResult second = bake::bake(low, high, bake::BakeMap::ObjectId, params64());
    REQUIRE(!first.image.pixels.empty());

    REQUIRE(first.image.pixels.size() == second.image.pixels.size());
    CHECK(first.image.pixels == second.image.pixels);
    REQUIRE(first.encoding.idColors.size() == second.encoding.idColors.size());
    for (std::size_t i = 0; i < first.encoding.idColors.size(); ++i) {
        CHECK(first.encoding.idColors[i].id == second.encoding.idColors[i].id);
        CHECK(first.encoding.idColors[i].color == second.encoding.idColors[i].color);
    }
    CHECK(first.encoding.idSource == second.encoding.idSource);
}

// ---- the shared map contract ----------------------------------------------

TEST_CASE("an unreached cage and an uncovered texel both take the reserved 'no id'") {
    const Mesh low = lowPlane();
    const Mesh high = targetWithColumn("material_id", {7, 42});
    bake::BakeParams p = params64();
    // A Target the cage cannot reach: the projection ray stops well short.
    Mesh distant = high;
    for (Index vi = 0; vi < distant.vertexCapacity(); ++vi) {
        const cyber::VertexId v{vi};
        if (distant.isAlive(v)) {
            distant.setPosition(v, distant.position(v) + Vec3{0, 0, -5.0f});
        }
    }
    const bake::BakeResult missed = bake::bake(low, distant, bake::BakeMap::MaterialId, p);
    REQUIRE(!missed.image.pixels.empty());
    CHECK(distinctColors(missed.image) == std::set<Rgb>{kNoId});
    // The table still names every Target id: the cage decides what is SEEN,
    // not what exists.
    CHECK(missed.encoding.idColors.size() == 2);

    // Opening the cage far enough brings the same Target back.
    p.cageDistance = 6.0f;
    const bake::BakeResult reached = bake::bake(low, distant, bake::BakeMap::MaterialId, p);
    REQUIRE(!reached.image.pixels.empty());
    CHECK(distinctColors(reached.image).count(kNoId) == 0);
}

TEST_CASE("a texel no chart covers pads with the reserved 'no id'") {
    // A missed cage ray and an unrasterised texel are two different code
    // paths to the same required value: the first writes "no id" while
    // shading, the second only ever holds the image's pre-fill. Both have to
    // be the reserved black, or the file would carry a colour that resolves
    // to no row of the reported table -- which is exactly what a consumer
    // uses the map for.
    const Mesh low = lowPlaneInQuarterLayout();
    const Mesh high = targetWithColumn("material_id", {7, 42});
    for (const bake::BakeMap map : {bake::BakeMap::MaterialId, bake::BakeMap::ObjectId}) {
        const bake::BakeResult result = bake::bake(low, high, map, params64());
        REQUIRE(!result.image.pixels.empty());
        // Inside the chart -- the rasteriser flips v, so the quarter layout
        // lands in the lower-left of UV space and the BOTTOM-left of the
        // image. Checked first, so a layout that covered nothing at all could
        // not pass this case vacuously.
        CHECK(byteAt(result.image, 10, 40) != kNoId);
        CHECK(byteAt(result.image, 20, 50) != kNoId);
        // Outside every chart: past it in u, past it in v, and in the corner.
        for (const std::pair<int, int>& at : {std::pair<int, int>{40, 40}, {10, 10}, {63, 10}}) {
            CHECK(byteAt(result.image, at.first, at.second) == kNoId);
            // Exactly zero in the float buffer too, not merely rounding to it.
            for (int c = 0; c < result.image.channels; ++c) {
                CHECK(result.image.at(at.first, at.second, c) == 0.0f);
            }
        }
    }
}

TEST_CASE("the id maps honour the texel ceiling, the UVs and cancellation") {
    const Mesh low = lowPlane();
    const Mesh high = targetWithColumn("material_id", {7, 42});
    for (const bake::BakeMap map : {bake::BakeMap::MaterialId, bake::BakeMap::ObjectId}) {
        bake::BakeParams over = params64();
        over.maxPixels = 64 * 64 - 1;
        CHECK(bake::bake(low, high, map, over).image.pixels.empty());

        Mesh noUvs = lowPlane();
        noUvs.cornerAttributes().remove("uv");
        CHECK(bake::bake(noUvs, high, map, params64()).image.pixels.empty());

        CancelToken token;
        token.requestCancel();
        const bake::BakeResult cancelled = bake::bake(low, high, map, params64(), nullptr, &token);
        CHECK(cancelled.cancelled);
    }
}

TEST_CASE("an id bake reports progress and completes") {
    const Mesh low = lowPlane();
    const Mesh high = targetWithColumn("material_id", {7, 42});
    std::vector<float> reported;
    cyber::ProgressSink sink(
        [&](float fraction, std::string_view) { reported.push_back(fraction); });
    const bake::BakeResult result =
        bake::bake(low, high, bake::BakeMap::MaterialId, params64(), &sink);
    REQUIRE(!result.image.pixels.empty());
    REQUIRE(!reported.empty());
    CHECK(reported.back() == doctest::Approx(1.0f));
}
