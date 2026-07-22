#include <doctest.h>

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

TEST_CASE("bake honors cooperative cancellation") {
    const Mesh low = makePlane(0, 0, 1, 0, 1, true, false);
    const Mesh high = makePlane(0, 0, 1, 0, 1, false, false);
    CancelToken cancel;
    cancel.requestCancel();
    const bake::BakeResult r =
        bake::bake(low, high, bake::BakeMap::Normal, params32(), nullptr, &cancel);
    REQUIRE(r.cancelled);
}
