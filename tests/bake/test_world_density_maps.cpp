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

// The left island alone, so a degenerate face's effect on the reported mean can
// be measured against the map that does not have one.
Mesh leftIslandOnly() {
    Mesh mesh = emptyWithUv();
    addQuad(mesh, {Vec3{0, 0, 0}, Vec3{1, 0, 0}, Vec3{1, 1, 0}, Vec3{0, 1, 0}}, 0.0f, 0.5f);
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
        // every other bake parameter already follows.
        p.placement = linear({1, 0, 0, 0, 0, 0, 0, 0, 1});
        CHECK(!bake::bake(mesh, mesh, bake::BakeMap::ObjectNormal, p).image.pixels.empty());
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
