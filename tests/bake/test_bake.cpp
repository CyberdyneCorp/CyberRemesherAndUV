#include <doctest.h>

#include <cmath>
#include <limits>
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
using cyber::VertexId;
namespace bake = cyber::bake;

namespace {

// Axis-aligned quad in the z = `z` plane spanning [x0,x1] x [y0,y1].
Mesh makePlane(float z, float x0, float x1, float y0, float y1, bool withUv, bool withColor,
               Vec3 color = {1, 1, 1}) {
    const std::vector<Vec3> p = {{x0, y0, z}, {x1, y0, z}, {x1, y1, z}, {x0, y1, z}};
    const std::vector<std::vector<Index>> f = {{0, 1, 2, 3}};
    Mesh mesh = Mesh::fromIndexed(p, f);
    if (withUv) {
        auto& uv = mesh.cornerAttributes().create<Vec2>("uv");
        for (Index fi = 0; fi < mesh.faceCapacity(); ++fi) {
            if (!mesh.isAlive(FaceId{fi})) {
                continue;
            }
            for (const LoopId l : mesh.faceLoops(FaceId{fi})) {
                const Vec3 pos = mesh.position(mesh.loopVertex(l));
                uv[l.value] = {pos.x, pos.y};  // xy == uv on the unit square
            }
        }
    }
    if (withColor) {
        auto& col = mesh.vertexAttributes().create<Vec3>("color");
        for (Index i = 0; i < mesh.vertexCapacity(); ++i) {
            col[i] = color;
        }
    }
    return mesh;
}

// Value at the layout centre (UV ~ 0.5, 0.5).
float center(const bake::Image& img, int c) { return img.at(img.width / 2, img.height / 2, c); }

bake::BakeParams params32() {
    bake::BakeParams p;
    p.width = 32;
    p.height = 32;
    p.cageDistance = 0.1f;
    return p;
}

}  // namespace

TEST_CASE("bake needs UVs and a non-empty target") {
    const Mesh low = makePlane(0, 0, 1, 0, 1, /*uv=*/false, false);
    const Mesh high = makePlane(0, 0, 1, 0, 1, false, false);
    const bake::BakeResult r = bake::bake(low, high, bake::BakeMap::Normal, params32());
    REQUIRE(r.image.pixels.empty());
    REQUIRE(r.texelsCovered == 0);
}

TEST_CASE("UV layout is rasterized to covered texels") {
    const Mesh low = makePlane(0, 0, 1, 0, 1, true, false);
    const Mesh high = makePlane(0, 0, 1, 0, 1, false, false);
    const bake::BakeResult r = bake::bake(low, high, bake::BakeMap::Position, params32());
    REQUIRE(r.image.width == 32);
    REQUIRE(r.texelsCovered > 900);  // ~full 32x32 square
}

TEST_CASE("normal bake of coincident flat surfaces is tangent-space up") {
    const Mesh low = makePlane(0, 0, 1, 0, 1, true, false);
    const Mesh high = makePlane(0, 0, 1, 0, 1, false, false);
    const bake::BakeResult r = bake::bake(low, high, bake::BakeMap::Normal, params32());
    // Tangent-space +Z encodes to (0.5, 0.5, 1).
    REQUIRE(center(r.image, 0) == doctest::Approx(0.5f).epsilon(0.02));
    REQUIRE(center(r.image, 1) == doctest::Approx(0.5f).epsilon(0.02));
    REQUIRE(center(r.image, 2) == doctest::Approx(1.0f).epsilon(0.02));
}

TEST_CASE("normal-map padding is the flat normal, and survives a DirectX green flip") {
    // Regression: uncovered texels were left at (0,0,0). Black is not a normal
    // — it bleeds into the surface under dilation/mips — and the unreal export
    // preset's green flip turned it into pure green (0,1,0), so one bake shipped
    // two different paddings depending on the target app. The flat normal is
    // invariant under the flip, which is the property that makes it correct.
    const Mesh low = makePlane(0, 0, 0.5f, 0, 0.5f, /*uv=*/true, false);  // covers a UV corner only
    const Mesh high = makePlane(0, 0, 0.5f, 0, 0.5f, false, false);
    const bake::BakeResult r = bake::bake(low, high, bake::BakeMap::Normal, params32());
    REQUIRE_FALSE(r.image.pixels.empty());

    // A texel far outside the layout is padding.
    const int px = r.image.width - 1;
    const int py = 0;
    CHECK(r.image.at(px, py, 0) == doctest::Approx(0.5f));
    CHECK(r.image.at(px, py, 1) == doctest::Approx(0.5f));
    CHECK(r.image.at(px, py, 2) == doctest::Approx(1.0f));
    // g -> 1 - g leaves it unchanged, so both conventions ship the same padding.
    CHECK(1.0f - r.image.at(px, py, 1) == doctest::Approx(r.image.at(px, py, 1)));
}

TEST_CASE("padding is each map's neutral value, never black") {
    // Regression: only the normal map pre-filled a neutral padding, so cavity
    // and AO shipped 0 (maximum concavity / full occlusion) and curvature
    // shipped 0 (maximum concavity) in the gutters. Without a coverage mask,
    // mip generation and dilation bleed that black inward at every chart
    // border — a phantom dark crease in the multiply slot, a ring in AO.
    const Mesh low = makePlane(0, 0, 0.5f, 0, 0.5f, /*uv=*/true, false);  // a UV corner only
    const Mesh high = makePlane(0, 0, 0.5f, 0, 0.5f, false, false);
    bake::BakeParams p = params32();
    p.aoSamples = 8;

    struct Case {
        bake::BakeMap map;
        float padding;
    };
    const std::vector<Case> cases = {
        {bake::BakeMap::Curvature, 0.5f},         // mid-gray: no curvature
        {bake::BakeMap::Cavity, 1.0f},            // white: no concavity
        {bake::BakeMap::AmbientOcclusion, 1.0f},  // fully open
        {bake::BakeMap::Displacement, 0.0f},      // zero height already IS neutral
    };
    for (const Case& c : cases) {
        CAPTURE(static_cast<int>(c.map));
        const bake::BakeResult r = bake::bake(low, high, c.map, p);
        REQUIRE_FALSE(r.image.pixels.empty());
        REQUIRE(r.texelsCovered > 0);  // the layout really is partial, not empty
        // Far outside the covered corner, and again just past its border: the
        // padding equals what the flat covered texels read, so the border is
        // continuous under a filter that ignores coverage.
        CHECK(r.image.at(r.image.width - 1, 0, 0) == doctest::Approx(c.padding));
        CHECK(r.image.at(r.image.width - 1, r.image.height - 1, 0) == doctest::Approx(c.padding));
    }
}

TEST_CASE("a non-finite UV cannot poison the baked image") {
    // Regression: the inside test is a `>` comparison on an expression a NaN UV
    // makes NaN, and `NaN > eps` is false — so the texel was ACCEPTED rather
    // than rejected and the bbox of the triangle's remaining finite corners was
    // written as NaN. A glTF TEXCOORD_0 accessor holding a NaN float reaches
    // the rasterizer verbatim.
    Mesh low = makePlane(0, 0, 1, 0, 1, /*uv=*/true, false);
    std::vector<Vec2>* uv = low.cornerAttributes().find<Vec2>("uv");
    REQUIRE(uv != nullptr);
    (*uv)[1] = {std::numeric_limits<float>::quiet_NaN(), std::numeric_limits<float>::quiet_NaN()};

    const Mesh high = makePlane(0, 0, 1, 0, 1, false, false);
    const bake::BakeResult r = bake::bake(low, high, bake::BakeMap::Position, params32());
    for (const float v : r.image.pixels) {
        REQUIRE(std::isfinite(v));
    }
    // Only the sub-triangle carrying the NaN corner is dropped, so the rest of
    // the layout still bakes — the guard rejects, it does not empty the map.
    REQUIRE(r.texelsCovered > 0);
}

TEST_CASE("displacement bake measures height of the target above the surface") {
    const Mesh low = makePlane(0.0f, 0, 1, 0, 1, true, false);
    const Mesh high = makePlane(0.05f, 0, 1, 0, 1, false, false);  // 0.05 above, within the cage
    const bake::BakeResult r = bake::bake(low, high, bake::BakeMap::Displacement, params32());
    REQUIRE(center(r.image, 0) == doctest::Approx(0.05f).epsilon(0.05));
}

TEST_CASE("position bake records the hit point") {
    const Mesh low = makePlane(0, 0, 1, 0, 1, true, false);
    const Mesh high = makePlane(0, 0, 1, 0, 1, false, false);
    const bake::BakeResult r = bake::bake(low, high, bake::BakeMap::Position, params32());
    REQUIRE(center(r.image, 0) == doctest::Approx(0.5f).epsilon(0.05));  // x
    REQUIRE(center(r.image, 1) == doctest::Approx(0.5f).epsilon(0.05));  // y
    REQUIRE(center(r.image, 2) == doctest::Approx(0.0f).epsilon(0.02));  // z on the plane
}

TEST_CASE("color bake samples the target vertex color") {
    const Mesh low = makePlane(0, 0, 1, 0, 1, true, false);
    const Mesh high = makePlane(0, 0, 1, 0, 1, false, /*color=*/true, Vec3{1, 0, 0});
    const bake::BakeResult r = bake::bake(low, high, bake::BakeMap::Color, params32());
    REQUIRE(center(r.image, 0) == doctest::Approx(1.0f).epsilon(0.02));  // red
    REQUIRE(center(r.image, 1) == doctest::Approx(0.0f).epsilon(0.02));
    REQUIRE(center(r.image, 2) == doctest::Approx(0.0f).epsilon(0.02));
}

TEST_CASE("ambient occlusion darkens under an occluder") {
    const Mesh low = makePlane(0, 0, 1, 0, 1, true, false);

    bake::BakeParams p = params32();
    p.aoSamples = 32;
    p.aoRadius = 1.0f;

    // Open: target coplanar below the hemisphere -> upward rays escape.
    const Mesh open = makePlane(0, 0, 1, 0, 1, false, false);
    const bake::BakeResult ro = bake::bake(low, open, bake::BakeMap::AmbientOcclusion, p);
    REQUIRE(center(ro.image, 0) > 0.85f);

    // Occluded: a large ceiling above catches the upward rays.
    const Mesh ceiling = makePlane(0.3f, -1, 2, -1, 2, false, false);
    const bake::BakeResult rc = bake::bake(low, ceiling, bake::BakeMap::AmbientOcclusion, p);
    REQUIRE(center(rc.image, 0) < 0.3f);
}

TEST_CASE("an AO bake with no ray budget is rejected instead of baked as NaN") {
    // Regression: aoSamples was never range-checked, so openness computed
    // `1 - occluded / 0` = NaN at every covered texel and the bake reported
    // success with texelsCovered > 0. NaN loses every ordered comparison, so
    // the PNG writer's clamp turned the whole map into byte 0 — solid black,
    // the inverse of AO's neutral white — and the caller was told nothing.
    const Mesh low = makePlane(0, 0, 1, 0, 1, /*uv=*/true, false);
    const Mesh high = makePlane(0, 0, 1, 0, 1, false, false);

    bake::BakeParams p = params32();
    p.aoSamples = 0;
    const bake::BakeResult zero = bake::bake(low, high, bake::BakeMap::AmbientOcclusion, p);
    CHECK(zero.image.pixels.empty());
    CHECK(zero.texelsCovered == 0);

    // A negative budget is the same caller bug one step further along: it used
    // to reach the ray buffers as an enormous allocation.
    p.aoSamples = -4;
    CHECK(bake::bake(low, high, bake::BakeMap::AmbientOcclusion, p).image.pixels.empty());

    // The boundary still bakes, and every float in it is finite.
    p.aoSamples = 1;
    const bake::BakeResult one = bake::bake(low, high, bake::BakeMap::AmbientOcclusion, p);
    REQUIRE(one.texelsCovered > 0);
    for (const float v : one.image.pixels) {
        REQUIRE(std::isfinite(v));
    }

    // aoSamples belongs to AO alone: a map that never spends a ray budget is
    // unaffected by a garbage one, exactly as before the check existed.
    p.aoSamples = 0;
    CHECK_FALSE(bake::bake(low, high, bake::BakeMap::Normal, p).image.pixels.empty());
}

TEST_CASE("a numeric bake parameter out of range yields an empty image") {
    // Every member the requested map reads is checked, not just aoSamples: a
    // non-finite distance seeds rays that can never hit anything, so the bake
    // would ship a blank map dressed as a success.
    const Mesh low = makePlane(0, 0, 1, 0, 1, /*uv=*/true, false);
    const Mesh high = makePlane(0, 0, 1, 0, 1, false, false);
    const float nan = std::numeric_limits<float>::quiet_NaN();
    const float inf = std::numeric_limits<float>::infinity();

    struct Case {
        const char* what;
        bake::BakeMap map;
        bake::BakeParams params;
    };
    auto with = [](auto&& set) {
        bake::BakeParams p = params32();
        p.aoSamples = 8;
        set(p);
        return p;
    };
    const std::vector<Case> cases = {
        {"cage NaN", bake::BakeMap::Normal,
         with([&](bake::BakeParams& p) { p.cageDistance = nan; })},
        {"cage inf", bake::BakeMap::Normal,
         with([&](bake::BakeParams& p) { p.cageDistance = inf; })},
        {"cage < 0", bake::BakeMap::Displacement,
         with([](bake::BakeParams& p) { p.cageDistance = -0.1f; })},
        {"aoRadius NaN", bake::BakeMap::AmbientOcclusion,
         with([&](bake::BakeParams& p) { p.aoRadius = nan; })},
        {"aoRadius < 0", bake::BakeMap::AmbientOcclusion,
         with([](bake::BakeParams& p) { p.aoRadius = -1.0f; })},
        {"aoBias NaN", bake::BakeMap::AmbientOcclusion,
         with([&](bake::BakeParams& p) { p.aoBias = nan; })},
        {"curvatureRange NaN", bake::BakeMap::Curvature,
         with([&](bake::BakeParams& p) { p.curvatureRange = nan; })},
    };
    for (const Case& c : cases) {
        INFO(c.what);
        const bake::BakeResult r = bake::bake(low, high, c.map, c.params);
        CHECK(r.image.pixels.empty());
        CHECK(r.texelsCovered == 0);
    }

    // The values these cases perturb are all in range in params32(), so the
    // control bakes — the check rejects a bad request, it does not reject bakes.
    bake::BakeParams ok = params32();
    ok.aoSamples = 8;
    CHECK_FALSE(bake::bake(low, high, bake::BakeMap::AmbientOcclusion, ok).image.pixels.empty());
    // A negative curvatureRange keeps its documented meaning (auto range).
    ok.curvatureRange = -1.0f;
    CHECK_FALSE(bake::bake(low, high, bake::BakeMap::Curvature, ok).image.pixels.empty());
}

TEST_CASE("bake honors cooperative cancellation") {
    const Mesh low = makePlane(0, 0, 1, 0, 1, true, false);
    const Mesh high = makePlane(0, 0, 1, 0, 1, false, false);
    CancelToken cancel;
    cancel.requestCancel();
    const bake::BakeResult r =
        bake::bake(low, high, bake::BakeMap::Normal, params32(), nullptr, &cancel);
    REQUIRE(r.cancelled);
}
