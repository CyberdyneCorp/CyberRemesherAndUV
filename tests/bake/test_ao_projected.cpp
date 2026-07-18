#include <doctest.h>

#include <cmath>
#include <vector>

#include "cyber/bake/bake.hpp"
#include "cyber/core/mesh.hpp"

using cyber::FaceId;
using cyber::Index;
using cyber::LoopId;
using cyber::Mesh;
using cyber::Vec2;
using cyber::Vec3;
namespace bake = cyber::bake;

namespace {

// Flat low-poly quad in the z=0 plane over [0,1]^2 with uv == xy.
Mesh lowPlane() {
    const std::vector<Vec3> p = {{0, 0, 0}, {1, 0, 0}, {1, 1, 0}, {0, 1, 0}};
    const std::vector<std::vector<Index>> f = {{0, 1, 2, 3}};
    Mesh mesh = Mesh::fromIndexed(p, f);
    auto& uv = mesh.cornerAttributes().create<Vec2>("uv");
    for (Index fi = 0; fi < mesh.faceCapacity(); ++fi) {
        if (!mesh.isAlive(FaceId{fi})) {
            continue;
        }
        for (const LoopId l : mesh.faceLoops(FaceId{fi})) {
            const Vec3 pos = mesh.position(mesh.loopVertex(l));
            uv[l.value] = {pos.x, pos.y};
        }
    }
    return mesh;
}

// High-poly: a flat base coplanar with the low-poly, plus a short vertical ridge
// wall standing at x=0.5 (its own vertices, so the base keeps a clean +z
// normal). A smooth low-poly baked against this should darken beside the ridge.
Mesh highWithRidge() {
    const std::vector<Vec3> p = {
        // base quad [0,1]^2 at z=0
        {0, 0, 0}, {1, 0, 0}, {1, 1, 0}, {0, 1, 0},
        // ridge wall at x=0.5, y in [0,1], z in [0,0.15]
        {0.5f, 0, 0}, {0.5f, 1, 0}, {0.5f, 1, 0.15f}, {0.5f, 0, 0.15f}};
    const std::vector<std::vector<Index>> f = {{0, 1, 2, 3}, {4, 5, 6, 7}};
    return Mesh::fromIndexed(p, f);
}

// AO openness sampled at layout UV (u, v); uv == xy for lowPlane().
float aoAt(const bake::Image& img, float u, float v) {
    int px = static_cast<int>(u * static_cast<float>(img.width));
    int py = static_cast<int>((1.0f - v) * static_cast<float>(img.height));
    px = std::max(0, std::min(img.width - 1, px));
    py = std::max(0, std::min(img.height - 1, py));
    return img.at(px, py, 0);
}

bake::BakeParams aoParams() {
    bake::BakeParams p;
    p.width = 64;
    p.height = 64;
    p.cageDistance = 0.1f;
    p.aoSamples = 64;
    p.aoRadius = 1.0f;
    return p;
}

}  // namespace

TEST_CASE("AO projects the low-poly texel onto the high-poly before sampling") {
    const Mesh low = lowPlane();
    const Mesh high = highWithRidge();
    const bake::BakeResult r =
        bake::bake(low, high, bake::BakeMap::AmbientOcclusion, aoParams());
    REQUIRE_FALSE(r.image.pixels.empty());

    // Beside the ridge (u ~ 0.51) the projected high-poly hit is occluded by the
    // wall; far from it (u ~ 0.1) the hemisphere is open. The whole low-poly is a
    // smooth plane, so any darkening comes from projecting to the high-poly.
    const float occluded = aoAt(r.image, 0.51f, 0.5f);
    const float open = aoAt(r.image, 0.1f, 0.5f);

    CHECK(open > 0.8f);            // open away from the ridge
    CHECK(occluded < open - 0.2f);  // measurably darker beside the ridge
}

TEST_CASE("AO self-occlusion of a flat mesh against itself stays open") {
    const Mesh low = lowPlane();
    const Mesh same = lowPlane();
    const bake::BakeResult r =
        bake::bake(low, same, bake::BakeMap::AmbientOcclusion, aoParams());
    REQUIRE_FALSE(r.image.pixels.empty());
    // Projection hits the coincident surface at ~0 distance -> fully open.
    CHECK(aoAt(r.image, 0.5f, 0.5f) > 0.9f);
}
