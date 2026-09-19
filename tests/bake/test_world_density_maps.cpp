#include <doctest.h>

#include <algorithm>
#include <array>
#include <cmath>
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

// The world-space direction and UV density maps (surface-baking spec:
// "World-space direction map and the placement transform", "UV density maps").
//
// The world map's whole reason to exist is the placement transform: this engine
// has ONE model space, so without one it would be the object-space normal map
// under a second name. Two of the cases below are therefore about the
// difference the transform makes, and one is about its ABSENCE -- an identity
// placement must reproduce object-normal at ZERO tolerance, which is the
// statement that the two maps differ by the transform and by nothing else.
namespace {

// A quad with its own UVs over a sub-rectangle of the layout. `u0`/`u1` let two
// of these sit side by side in one map without sharing a texel.
void addQuad(Mesh& mesh, const std::array<Vec3, 4>& corners, float u0, float u1) {
    std::vector<cyber::VertexId> verts;
    verts.reserve(4);
    for (const Vec3& p : corners) {
        verts.push_back(mesh.addVertex(p));
    }
    const FaceId face = mesh.addFace(verts);
    auto* uv = mesh.cornerAttributes().find<Vec2>("uv");
    REQUIRE(uv != nullptr);
    const std::array<Vec2, 4> layout{Vec2{u0, 0.0f}, Vec2{u1, 0.0f}, Vec2{u1, 1.0f},
                                     Vec2{u0, 1.0f}};
    std::size_t corner = 0;
    for (const LoopId l : mesh.faceLoops(face)) {
        (*uv)[l.value] = layout[corner++];
    }
}

Mesh emptyWithUv() {
    Mesh mesh;
    mesh.cornerAttributes().create<Vec2>("uv");
    return mesh;
}

// Two quads meeting along x = 1, the second tilted, so the baked normal VARIES
// across the layout: a map that is one flat colour cannot tell a correct
// transform from a wrong one.
Mesh tent() {
    Mesh mesh = emptyWithUv();
    addQuad(mesh, {Vec3{0, 0, 0}, Vec3{1, 0, 0}, Vec3{1, 1, 0}, Vec3{0, 1, 0}}, 0.0f, 0.5f);
    addQuad(mesh, {Vec3{1, 0, 0}, Vec3{2, 0, 1}, Vec3{2, 1, 1}, Vec3{1, 1, 0}}, 0.5f, 1.0f);
    return mesh;
}

// One planar quad whose normal is (0, -1, 1) / sqrt(2): deliberately NOT
// axis-aligned, because a scale along an axis the normal has no component on
// cannot tell an inverse transpose from a plain multiply.
Mesh tilted() {
    Mesh mesh = emptyWithUv();
    addQuad(mesh, {Vec3{0, 0, 0}, Vec3{1, 0, 0}, Vec3{1, 1, 1}, Vec3{0, 1, 1}}, 0.0f, 1.0f);
    return mesh;
}

// One quad of surface area 4 over the WHOLE layout: 1 UV unit over 4 model
// units, so the absolute density is exactly (width * height) / 4.
Mesh wholeLayoutQuad() {
    Mesh mesh = emptyWithUv();
    addQuad(mesh, {Vec3{0, 0, 0}, Vec3{2, 0, 0}, Vec3{2, 2, 0}, Vec3{0, 2, 0}}, 0.0f, 1.0f);
    return mesh;
}

// Two islands of EQUAL UV area over surfaces of area 1 and 4: the left is packed
// four times as densely as the right, and each covers exactly half the layout.
Mesh unevenIslands() {
    Mesh mesh = emptyWithUv();
    addQuad(mesh, {Vec3{0, 0, 0}, Vec3{1, 0, 0}, Vec3{1, 1, 0}, Vec3{0, 1, 0}}, 0.0f, 0.5f);
    addQuad(mesh, {Vec3{3, 0, 0}, Vec3{5, 0, 0}, Vec3{5, 2, 0}, Vec3{3, 2, 0}}, 0.5f, 1.0f);
    return mesh;
}

// The left island again, beside a face whose four corners are COLLINEAR: it has
// UV area and no surface area, so its density is the undefined ratio the spec
// gives a sentinel to.
Mesh islandBesideDegenerateFace() {
    Mesh mesh = emptyWithUv();
    addQuad(mesh, {Vec3{0, 0, 0}, Vec3{1, 0, 0}, Vec3{1, 1, 0}, Vec3{0, 1, 0}}, 0.0f, 0.5f);
    addQuad(mesh, {Vec3{3, 0, 0}, Vec3{4, 0, 0}, Vec3{5, 0, 0}, Vec3{6, 0, 0}}, 0.5f, 1.0f);
    return mesh;
}

// The left island again, beside a face whose four UV corners are COLLINEAR: it
// has surface area and no UV AREA, which is the other half of the degenerate
// case -- and, unlike the collinear-in-3D face above, it rasterizes to no texel
// at all, so the sentinel it takes is the uncovered background's.
Mesh islandBesideZeroUvAreaFace() {
    Mesh mesh = emptyWithUv();
    addQuad(mesh, {Vec3{0, 0, 0}, Vec3{1, 0, 0}, Vec3{1, 1, 0}, Vec3{0, 1, 0}}, 0.0f, 0.5f);
    // u0 == u1: the whole face collapses onto one UV line.
    addQuad(mesh, {Vec3{3, 0, 0}, Vec3{4, 0, 0}, Vec3{4, 1, 0}, Vec3{3, 1, 0}}, 0.75f, 0.75f);
    return mesh;
}

// The left island alone, so a degenerate face's effect on the reported mean can
// be measured against the map that does not have one. It covers the LEFT HALF
// of the layout and nothing else, which is what makes it the mesh to measure
// the mean's treatment of BACKGROUND texels on: half the map is uncovered.
Mesh leftIslandOnly() {
    Mesh mesh = emptyWithUv();
    addQuad(mesh, {Vec3{0, 0, 0}, Vec3{1, 0, 0}, Vec3{1, 1, 0}, Vec3{0, 1, 0}}, 0.0f, 0.5f);
    return mesh;
}

// A flat quad over the whole layout, normal +z. The EditMesh of the cage case.
Mesh flatQuad() {
    Mesh mesh = emptyWithUv();
    addQuad(mesh, {Vec3{0, 0, 0}, Vec3{1, 0, 0}, Vec3{1, 1, 0}, Vec3{0, 1, 0}}, 0.0f, 1.0f);
    return mesh;
}

// A Target floating just above that quad and tilted, so its normal is
// (0, -0.2, 1) normalized rather than +z: a cage long enough to reach it bakes
// ITS normal, and a cage that is not falls back to the EditMesh's own.
Mesh floatingTarget() {
    Mesh mesh = emptyWithUv();
    addQuad(mesh, {Vec3{0, 0, 0.2f}, Vec3{1, 0, 0.2f}, Vec3{1, 1, 0.4f}, Vec3{0, 1, 0.4f}}, 0.0f,
            1.0f);
    return mesh;
}

bake::BakeParams params64() {
    bake::BakeParams p;
    p.width = 64;
    p.height = 64;
    p.cageDistance = 0.05f;
    p.paddingRadius = 0;  // the maps' own values, before any band is grown
    return p;
}

// A row-major 4x4 from its linear part; the translation is never read by a
// direction map and is left at zero.
bake::PlacementMatrix linear(const std::array<float, 9>& m) {
    return bake::PlacementMatrix{m[0], m[1], m[2], 0, m[3], m[4], m[5], 0,
                                 m[6], m[7], m[8], 0, 0,    0,    0,    1};
}

Vec3 decode(const bake::Image& image, int px, int py) {
    return Vec3{image.at(px, py, 0) * 2.0f - 1.0f, image.at(px, py, 1) * 2.0f - 1.0f,
                image.at(px, py, 2) * 2.0f - 1.0f};
}

float sample(const bake::Image& image, float u, float v) {
    const int px =
        std::clamp(static_cast<int>(u * static_cast<float>(image.width)), 0, image.width - 1);
    const int py = std::clamp(static_cast<int>((1.0f - v) * static_cast<float>(image.height)), 0,
                              image.height - 1);
    return image.at(px, py, 0);
}

// The texels the bake actually wrote a direction into, which is where a
// per-texel comparison is meaningful: an uncovered texel holds the neutral
// padding value in both maps and would agree for the wrong reason.
bool covered(const bake::Image& image, int px, int py) {
    const Vec3 v = decode(image, px, py);
    return cyber::length(v) > 0.5f;
}

}  // namespace

TEST_CASE("an identity placement reproduces the object-space normal map exactly") {
    const Mesh mesh = tent();
    const bake::BakeParams p = params64();
    REQUIRE(p.placement == bake::identityPlacement());

    const bake::BakeResult object = bake::bake(mesh, mesh, bake::BakeMap::ObjectNormal, p);
    const bake::BakeResult world = bake::bake(mesh, mesh, bake::BakeMap::WorldDirection, p);
    REQUIRE(!object.image.pixels.empty());
    REQUIRE(world.image.pixels.size() == object.image.pixels.size());

    // Texel for texel, at ZERO tolerance. The transform is skipped outright for
    // an identity linear part precisely so this can be asserted as equality:
    // renormalizing an already-unit vector is not the identity in float.
    CHECK(world.image.pixels == object.image.pixels);
    CHECK(world.encoding.basis == bake::EncodingBasis::WorldDirection);
    CHECK(object.encoding.basis == bake::EncodingBasis::ObjectNormal);
    CHECK(world.encoding.placement == bake::identityPlacement());
}

TEST_CASE("a placement rotation turns the world direction map") {
    const Mesh mesh = tent();
    bake::BakeParams p = params64();
    const bake::BakeResult identity = bake::bake(mesh, mesh, bake::BakeMap::WorldDirection, p);

    // A quarter turn about X: y -> z, z -> -y.
    p.placement = linear({1, 0, 0, 0, 0, -1, 0, 1, 0});
    const bake::BakeResult rotated = bake::bake(mesh, mesh, bake::BakeMap::WorldDirection, p);
    REQUIRE(!rotated.image.pixels.empty());
    CHECK(rotated.image.pixels != identity.image.pixels);
    CHECK(rotated.encoding.placement == p.placement);

    int compared = 0;
    for (int py = 0; py < rotated.image.height; ++py) {
        for (int px = 0; px < rotated.image.width; ++px) {
            if (!covered(identity.image, px, py)) {
                continue;
            }
            const Vec3 before = decode(identity.image, px, py);
            const Vec3 after = decode(rotated.image, px, py);
            const Vec3 want{before.x, -before.z, before.y};
            CHECK(std::fabs(after.x - want.x) < 1e-4f);
            CHECK(std::fabs(after.y - want.y) < 1e-4f);
            CHECK(std::fabs(after.z - want.z) < 1e-4f);
            ++compared;
        }
    }
    CHECK(compared > 100);
}

TEST_CASE("a non-uniform scale carries the normal by the inverse transpose") {
    const Mesh mesh = tilted();
    bake::BakeParams p = params64();
    // Doubling Y. The surface's own normal is (0, -1, 1)/sqrt(2); the PLACED
    // surface's normal is (0, -1, 2)/sqrt(5), which is what the inverse
    // transpose diag(1, 0.5, 1) produces. A plain multiply would give
    // (0, -2, 1)/sqrt(5) -- the two are each other's swapped components, so the
    // wrong one cannot pass this by accident.
    p.placement = linear({1, 0, 0, 0, 2, 0, 0, 0, 1});
    const bake::BakeResult world = bake::bake(mesh, mesh, bake::BakeMap::WorldDirection, p);
    REQUIRE(!world.image.pixels.empty());

    const Vec3 want = cyber::normalized(Vec3{0.0f, -1.0f, 2.0f});
    const Vec3 plainMultiply = cyber::normalized(Vec3{0.0f, -2.0f, 1.0f});
    const Vec3 got = decode(world.image, world.image.width / 2, world.image.height / 2);
    CHECK(std::fabs(got.x - want.x) < 1e-4f);
    CHECK(std::fabs(got.y - want.y) < 1e-4f);
    CHECK(std::fabs(got.z - want.z) < 1e-4f);
    CHECK(std::fabs(got.y - plainMultiply.y) > 0.1f);
    CHECK(cyber::length(got) == doctest::Approx(1.0f).epsilon(1e-4));
}

TEST_CASE("the placement is applied in object space and the up axis re-expresses the result") {
    // The ORDER of the two transforms, which is invisible under an identity
    // placement and invisible under a pure rotation as well -- a rotation about
    // X and the z-up swizzle commute. It takes a NON-UNIFORM SCALE plus z-up to
    // separate them, and this is the only case in the suite that combines the
    // two. Getting the order wrong reaches real output silently: the export
    // bundle selects z-up automatically for a z-up preset.
    const Mesh mesh = tilted();
    bake::BakeParams p = params64();
    p.upAxis = bake::UpAxis::ZUp;
    p.placement = linear({1, 0, 0, 0, 2, 0, 0, 0, 1});  // doubling Y
    const bake::BakeResult world = bake::bake(mesh, mesh, bake::BakeMap::WorldDirection, p);
    REQUIRE(!world.image.pixels.empty());

    // Placement FIRST: the object normal (0, -1, 1)/sqrt2 carried by the inverse
    // transpose diag(1, 0.5, 1) is (0, -1, 2)/sqrt5, and z-up re-expresses that
    // as (x, -z, y) = (0, -2, -1)/sqrt5.
    const Vec3 want = cyber::normalized(Vec3{0.0f, -2.0f, -1.0f});
    // Up axis FIRST would swizzle the object normal to (0, -1, -1)/sqrt2 and
    // then scale it into (0, -1, -2)/sqrt5 -- the same two components, swapped,
    // which is why this comparison cannot pass by accident.
    const Vec3 axisFirst = cyber::normalized(Vec3{0.0f, -1.0f, -2.0f});
    const Vec3 got = decode(world.image, world.image.width / 2, world.image.height / 2);
    CHECK(std::fabs(got.x - want.x) < 1e-4f);
    CHECK(std::fabs(got.y - want.y) < 1e-4f);
    CHECK(std::fabs(got.z - want.z) < 1e-4f);
    CHECK(std::fabs(got.y - axisFirst.y) > 0.1f);
    CHECK(std::fabs(got.z - axisFirst.z) > 0.1f);
    CHECK(world.encoding.upAxis == bake::UpAxis::ZUp);
}

TEST_CASE("a singular or non-finite placement is refused, not defaulted") {
    const Mesh mesh = tent();
    bake::BakeParams p = params64();

    SUBCASE("a singular linear part") {
        p.placement = linear({1, 0, 0, 0, 0, 0, 0, 0, 1});  // flattens Y: no inverse
        CHECK(!bake::placementUsable(p.placement));
        CHECK(bake::bake(mesh, mesh, bake::BakeMap::WorldDirection, p).image.pixels.empty());
    }
    SUBCASE("a non-finite element") {
        p.placement[5] = std::numeric_limits<float>::quiet_NaN();
        CHECK(!bake::placementUsable(p.placement));
        CHECK(bake::bake(mesh, mesh, bake::BakeMap::WorldDirection, p).image.pixels.empty());
    }
    SUBCASE("but only for the map that reads it") {
        // A map that never looks at the placement is not refused because of one:
        // "parameters the requested map never reads stay unchecked" is the rule
        // every other bake parameter already follows, and here it is also what
        // keeps a caller that leaves this appended field alone -- an all-zero,
        // and therefore singular, matrix -- baking every map it always could.
        p.placement = linear({1, 0, 0, 0, 0, 0, 0, 0, 1});
        for (const bake::BakeMap map :
             {bake::BakeMap::ObjectNormal, bake::BakeMap::Normal, bake::BakeMap::UvDensity}) {
            CHECK(!bake::mapReadsPlacement(map));
            const bake::BakeResult result = bake::bake(mesh, mesh, map, p);
            CHECK(!result.image.pixels.empty());
            // And it is the IDENTITY they record, not the unusable matrix they
            // were handed: a consumer decodes with what it reads here.
            CHECK(result.encoding.placement == bake::identityPlacement());
        }
        CHECK(bake::mapReadsPlacement(bake::BakeMap::WorldDirection));
    }
}

TEST_CASE("a placement changes no map but the world direction map") {
    const Mesh mesh = tent();
    bake::BakeParams identity = params64();
    bake::BakeParams placed = params64();
    placed.placement = linear({1, 0, 0, 0, 0, -1, 0, 1, 0});

    for (const bake::BakeMap map : {bake::BakeMap::ObjectNormal, bake::BakeMap::ObjectPosition,
                                    bake::BakeMap::Position, bake::BakeMap::Normal}) {
        const bake::BakeResult before = bake::bake(mesh, mesh, map, identity);
        const bake::BakeResult after = bake::bake(mesh, mesh, map, placed);
        REQUIRE(!before.image.pixels.empty());
        CHECK(after.image.pixels == before.image.pixels);
        CHECK(after.encoding.placement == bake::identityPlacement());
    }
}

TEST_CASE("absolute UV density is texels per unit of surface area") {
    const Mesh mesh = wholeLayoutQuad();  // 1 UV unit over 4 model units
    bake::BakeParams p = params64();
    const bake::BakeResult result = bake::bake(mesh, mesh, bake::BakeMap::UvDensity, p);
    REQUIRE(!result.image.pixels.empty());
    CHECK(result.image.channels == 1);
    CHECK(result.encoding.basis == bake::EncodingBasis::UvDensity);
    CHECK(result.encoding.densityNormalization == bake::DensityNormalization::Absolute);

    const float want = 64.0f * 64.0f / 4.0f;
    CHECK(sample(result.image, 0.5f, 0.5f) == doctest::Approx(want));
    CHECK(sample(result.image, 0.1f, 0.9f) == doctest::Approx(want));
    CHECK(result.encoding.densityMean == doctest::Approx(want));

    // Four times the texels over the same surface is four times the density.
    p.width = 128;
    p.height = 128;
    const bake::BakeResult finer = bake::bake(mesh, mesh, bake::BakeMap::UvDensity, p);
    CHECK(sample(finer.image, 0.5f, 0.5f) == doctest::Approx(want * 4.0f));
}

TEST_CASE("the density mean is taken from the DEFINED texels, not the whole image") {
    // Half the layout is covered and half is background. Every other density
    // case here fills the whole UV square, where a mean over all 4096 texels and
    // a mean over the defined ones agree -- so this is the case that says the
    // background is excluded, and it is worth stating as an exact number rather
    // than as a comparison against another map that would carry the same bias.
    const Mesh mesh = leftIslandOnly();  // UV area 0.5 over surface area 1
    bake::BakeParams p = params64();
    const bake::BakeResult absolute = bake::bake(mesh, mesh, bake::BakeMap::UvDensity, p);
    REQUIRE(!absolute.image.pixels.empty());

    const float want = 0.5f * 64.0f * 64.0f;  // 2048 texels per square model unit
    CHECK(sample(absolute.image, 0.25f, 0.5f) == doctest::Approx(want));
    CHECK(sample(absolute.image, 0.75f, 0.5f) == 0.0f);  // uncovered: the sentinel

    std::size_t background = 0;
    for (const float value : absolute.image.pixels) {
        if (value == 0.0f) {
            ++background;
        }
    }
    // Roughly half the image, and the point is only that there IS background
    // for a mean over the whole image to be dragged down by.
    CHECK(background > absolute.image.pixels.size() / 4);
    // The mean of the DEFINED texels, which is `want` itself -- NOT the mean
    // over the image, which the background would halve.
    CHECK(absolute.encoding.densityMean == doctest::Approx(want));
    CHECK(absolute.encoding.densityMean > 0.9f * want);

    // And the relative map divides by that same defined-texel mean, so a
    // uniformly packed island reads exactly 1 however much background surrounds
    // it. A mean diluted by the background would put it at 2.
    p.densityNormalization = bake::DensityNormalization::Relative;
    const bake::BakeResult relative = bake::bake(mesh, mesh, bake::BakeMap::UvDensity, p);
    REQUIRE(!relative.image.pixels.empty());
    CHECK(relative.encoding.densityMean == doctest::Approx(want));
    CHECK(sample(relative.image, 0.25f, 0.5f) == doctest::Approx(1.0f));
    CHECK(sample(relative.image, 0.75f, 0.5f) == 0.0f);
}

TEST_CASE("a face with zero UV AREA writes no texel and leaves the mean alone") {
    // The other degenerate face: real surface area, no UV area. It rasterizes to
    // NOTHING -- a triangle of zero UV area covers no texel centre -- so its
    // region of the map reads the uncovered background, which is the same zero
    // the sentinel uses. That is what makes "no density here" read alike either
    // way, and it is why the map is still finite everywhere.
    const Mesh mesh = islandBesideZeroUvAreaFace();
    bake::BakeParams p = params64();
    p.densityNormalization = bake::DensityNormalization::Relative;
    const bake::BakeResult result = bake::bake(mesh, mesh, bake::BakeMap::UvDensity, p);
    REQUIRE(!result.image.pixels.empty());

    for (const float value : result.image.pixels) {
        CHECK(std::isfinite(value));
        CHECK(value >= 0.0f);
    }
    // Nothing anywhere in the collapsed face's column of the layout.
    for (int py = 0; py < result.image.height; ++py) {
        CHECK(result.image.at(result.image.width * 3 / 4, py, 0) == 0.0f);
    }

    // The reported mean is the left island's own, unchanged by the face that
    // wrote nothing.
    const Mesh alone = leftIslandOnly();
    const bake::BakeResult reference = bake::bake(alone, alone, bake::BakeMap::UvDensity, p);
    CHECK(result.encoding.densityMean == doctest::Approx(0.5f * 64.0f * 64.0f));
    CHECK(result.encoding.densityMean == doctest::Approx(reference.encoding.densityMean));
}

TEST_CASE("UV density shows an unevenly packed layout") {
    const Mesh mesh = unevenIslands();
    const bake::BakeParams p = params64();
    const bake::BakeResult result = bake::bake(mesh, mesh, bake::BakeMap::UvDensity, p);
    REQUIRE(!result.image.pixels.empty());

    const float dense = sample(result.image, 0.25f, 0.5f);
    const float sparse = sample(result.image, 0.75f, 0.5f);
    CHECK(dense > 0.0f);
    CHECK(sparse > 0.0f);
    CHECK(dense == doctest::Approx(sparse * 4.0f));
    CHECK(dense == doctest::Approx(0.5f * 64.0f * 64.0f));
}

TEST_CASE("relative UV density divides by the map's own mean") {
    const Mesh mesh = unevenIslands();
    bake::BakeParams p = params64();
    const bake::BakeResult absolute = bake::bake(mesh, mesh, bake::BakeMap::UvDensity, p);

    p.densityNormalization = bake::DensityNormalization::Relative;
    const bake::BakeResult relative = bake::bake(mesh, mesh, bake::BakeMap::UvDensity, p);
    REQUIRE(!relative.image.pixels.empty());
    CHECK(relative.encoding.densityNormalization == bake::DensityNormalization::Relative);
    // The mean is reported in BOTH modes, and it is the ABSOLUTE one, which is
    // what makes the relative map convertible back.
    CHECK(relative.encoding.densityMean == doctest::Approx(absolute.encoding.densityMean));

    const float mean = absolute.encoding.densityMean;
    CHECK(sample(relative.image, 0.25f, 0.5f) ==
          doctest::Approx(sample(absolute.image, 0.25f, 0.5f) / mean));
    CHECK(sample(relative.image, 0.75f, 0.5f) ==
          doctest::Approx(sample(absolute.image, 0.75f, 0.5f) / mean));
    // The two islands cover equal halves of the layout, so the mean sits
    // between them and the relative map straddles 1.
    CHECK(sample(relative.image, 0.25f, 0.5f) > 1.0f);
    CHECK(sample(relative.image, 0.75f, 0.5f) < 1.0f);
}

TEST_CASE("a degenerate face takes the density sentinel and does not poison the mean") {
    const Mesh mesh = islandBesideDegenerateFace();
    bake::BakeParams p = params64();
    p.densityNormalization = bake::DensityNormalization::Relative;
    const bake::BakeResult result = bake::bake(mesh, mesh, bake::BakeMap::UvDensity, p);
    REQUIRE(!result.image.pixels.empty());

    // Exactly zero, and nothing anywhere is an infinity or a NaN -- which is the
    // failure the sentinel exists for: either would make the mean below useless
    // and would survive into the written file as a plausible-looking number.
    CHECK(sample(result.image, 0.75f, 0.5f) == 0.0f);
    for (const float value : result.image.pixels) {
        CHECK(std::isfinite(value));
        CHECK(value >= 0.0f);
    }

    // The mean is the mean of the DEFINED texels alone: the same one the map
    // without the degenerate face reports.
    const Mesh alone = leftIslandOnly();
    const bake::BakeResult reference = bake::bake(alone, alone, bake::BakeMap::UvDensity, p);
    CHECK(result.encoding.densityMean == doctest::Approx(reference.encoding.densityMean));
    CHECK(result.encoding.densityMean > 0.0f);
}

TEST_CASE("UV density does not read the Target") {
    const Mesh mesh = unevenIslands();
    const Mesh otherTarget = tent();
    const bake::BakeParams p = params64();
    const bake::BakeResult first = bake::bake(mesh, mesh, bake::BakeMap::UvDensity, p);
    const bake::BakeResult second = bake::bake(mesh, otherTarget, bake::BakeMap::UvDensity, p);
    REQUIRE(!first.image.pixels.empty());
    CHECK(second.image.pixels == first.image.pixels);
}

TEST_CASE("the padded band follows each new map's channel semantics") {
    const Mesh mesh = leftIslandOnly();  // half the layout is background to pad into
    bake::BakeParams p = params64();
    p.paddingRadius = 6;

    SUBCASE("a world direction's band decodes to unit directions inside [0,1]") {
        const bake::BakeResult result = bake::bake(mesh, mesh, bake::BakeMap::WorldDirection, p);
        REQUIRE(!result.image.pixels.empty());
        CHECK(result.padding.mode == bake::PaddingMode::ExtrapolateUnit);
        CHECK(result.padding.texelsFilled > 0);
        for (const float value : result.image.pixels) {
            CHECK(value >= 0.0f);
            CHECK(value <= 1.0f);
        }
        // Just outside the island: a padded texel, and still a unit direction.
        const Vec3 band = decode(result.image, result.image.width / 2 + 2, result.image.height / 2);
        CHECK(cyber::length(band) == doctest::Approx(1.0f).epsilon(1e-4));
    }

    SUBCASE("a density's band is extrapolated, never renormalized, never negative") {
        const bake::BakeResult result = bake::bake(mesh, mesh, bake::BakeMap::UvDensity, p);
        REQUIRE(!result.image.pixels.empty());
        CHECK(result.padding.mode == bake::PaddingMode::Extrapolate);
        CHECK(result.padding.texelsFilled > 0);
        CHECK(result.encoding.valueMin == 0.0f);
        // Unbounded above: a density is a ratio, not a fraction, and declaring
        // [0,1] here would clamp a real map's band down to nothing.
        CHECK(std::isinf(result.encoding.valueMax));
        for (const float value : result.image.pixels) {
            CHECK(value >= 0.0f);
        }
        // The band holds the map's own magnitude rather than a unit-normalized
        // one: a scalar has no length to restore.
        const float band = sample(result.image, 0.52f, 0.5f);
        CHECK(band > 1.0f);
    }
}

TEST_CASE("the world map reads the projection cage and the density map does not") {
    // "Honours the same projection cage" is an acceptance claim for the world
    // map, and the density map's whole contract is the opposite: it is a
    // property of the UV layout, so the cage must not move it at all. Both
    // halves are asserted here, on the same pair of meshes, because either one
    // alone would leave the other free to be wrong.
    const Mesh mesh = flatQuad();
    const Mesh target = floatingTarget();
    bake::BakeParams shortCage = params64();
    shortCage.cageDistance = 0.05f;  // stops short of the Target
    bake::BakeParams longCage = params64();
    longCage.cageDistance = 0.5f;  // reaches it

    const bake::BakeResult missed =
        bake::bake(mesh, target, bake::BakeMap::WorldDirection, shortCage);
    const bake::BakeResult hit = bake::bake(mesh, target, bake::BakeMap::WorldDirection, longCage);
    REQUIRE(!missed.image.pixels.empty());
    REQUIRE(!hit.image.pixels.empty());

    // Missed: the EditMesh's own +z, which is the documented fallback.
    const Vec3 fallback = decode(missed.image, 32, 32);
    CHECK(std::fabs(fallback.y) < 1e-3f);
    CHECK(fallback.z == doctest::Approx(1.0f).epsilon(1e-3));
    // Hit: the TARGET's normal, which the short cage never saw.
    const Vec3 want = cyber::normalized(Vec3{0.0f, -0.2f, 1.0f});
    const Vec3 got = decode(hit.image, 32, 32);
    CHECK(std::fabs(got.y - want.y) < 1e-3f);
    CHECK(std::fabs(got.z - want.z) < 1e-3f);
    CHECK(hit.image.pixels != missed.image.pixels);

    // The density map is untouched by the same change: it casts the cage ray on
    // the shared path but reads nothing from where it lands.
    const bake::BakeResult near = bake::bake(mesh, target, bake::BakeMap::UvDensity, shortCage);
    const bake::BakeResult far = bake::bake(mesh, target, bake::BakeMap::UvDensity, longCage);
    REQUIRE(!near.image.pixels.empty());
    CHECK(far.image.pixels == near.image.pixels);
    CHECK(near.encoding.densityMean == doctest::Approx(64.0f * 64.0f));
}

TEST_CASE("both new maps take the shared bake path") {
    const Mesh mesh = unevenIslands();
    for (const bake::BakeMap map : {bake::BakeMap::WorldDirection, bake::BakeMap::UvDensity}) {
        bake::BakeParams p = params64();

        // The host's texel ceiling refuses the request before anything is
        // allocated, exactly as it does for every other map.
        p.maxPixels = 64 * 64 - 1;
        CHECK(bake::bake(mesh, mesh, map, p).image.pixels.empty());

        // A negative padding radius is refused rather than defaulted.
        p.maxPixels = 0;
        p.paddingRadius = -1;
        CHECK(bake::bake(mesh, mesh, map, p).image.pixels.empty());

        // Cooperative cancellation leaves no image.
        p.paddingRadius = 8;
        CancelToken token;
        token.requestCancel();
        const bake::BakeResult cancelled = bake::bake(mesh, mesh, map, p, nullptr, &token);
        CHECK(cancelled.cancelled);
    }
}
