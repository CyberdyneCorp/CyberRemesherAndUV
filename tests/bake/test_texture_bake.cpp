#include <doctest.h>

#include <cstddef>
#include <vector>

#include "cyber/bake/bake.hpp"
#include "cyber/core/mesh.hpp"
#include "cyber/imageio/load.hpp"

using cyber::FaceId;
using cyber::Index;
using cyber::LoopId;
using cyber::Mesh;
using cyber::Vec2;
using cyber::Vec3;
namespace bake = cyber::bake;
namespace imageio = cyber::imageio;

namespace {

// Axis-aligned quad in the z = 0 plane spanning the unit square, optionally
// carrying per-corner "uv" (uv == xy) and a uniform per-vertex "color".
Mesh makePlane(bool withUv, bool withColor, Vec3 color = {1, 1, 1}) {
    const std::vector<Vec3> p = {{0, 0, 0}, {1, 0, 0}, {1, 1, 0}, {0, 1, 0}};
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
                uv[l.value] = {pos.x, pos.y};
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

// 2x2 checker: texels (0,0)/(1,1) red, (1,0)/(0,1) green. RGB, row-major.
imageio::LoadedImage checker2x2() {
    imageio::LoadedImage img;
    img.width = 2;
    img.height = 2;
    img.channels = 3;
    const Vec3 r{1, 0, 0};
    const Vec3 g{0, 1, 0};
    const Vec3 texels[4] = {r, g, g, r};  // (0,0),(1,0),(0,1),(1,1)
    img.pixels.resize(2 * 2 * 3);
    for (int i = 0; i < 4; ++i) {
        img.pixels[static_cast<std::size_t>(i) * 3 + 0] = texels[i].x;
        img.pixels[static_cast<std::size_t>(i) * 3 + 1] = texels[i].y;
        img.pixels[static_cast<std::size_t>(i) * 3 + 2] = texels[i].z;
    }
    return img;
}

bake::BakeParams params32() {
    bake::BakeParams p;
    p.width = 32;
    p.height = 32;
    p.cageDistance = 0.1f;
    return p;
}

Vec3 pixel(const bake::Image& img, int x, int y) {
    return {img.at(x, y, 0), img.at(x, y, 1), img.at(x, y, 2)};
}

bool rgbEqual(Vec3 a, Vec3 b) {
    return a.x == doctest::Approx(b.x).epsilon(0.02) && a.y == doctest::Approx(b.y).epsilon(0.02) &&
           a.z == doctest::Approx(b.z).epsilon(0.02);
}

bool isRed(Vec3 c) { return c.x > 0.8f && c.y < 0.2f; }
bool isGreen(Vec3 c) { return c.y > 0.8f && c.x < 0.2f; }

}  // namespace

TEST_CASE("color bake samples a target texture at the interpolated Target UV") {
    const Mesh low = makePlane(/*uv=*/true, /*color=*/false);
    const Mesh high = makePlane(/*uv=*/true, /*color=*/false);  // Target carries UVs
    const imageio::LoadedImage tex = checker2x2();

    bake::BakeParams p = params32();
    p.colorSource = bake::ColorSource{bake::ColorSource::Texture, &tex};
    const bake::BakeResult r = bake::bake(low, high, bake::BakeMap::Color, p);
    REQUIRE(r.image.width == 32);

    // Four texels near the UV quadrant centres. Pixels 7 and 24 map to UVs that
    // clamp onto the 2x2 texel centres, so bilinear returns the pure texel.
    const Vec3 topLeft = pixel(r.image, 7, 7);     // u<0.5, v>0.5
    const Vec3 topRight = pixel(r.image, 24, 7);   // u>0.5, v>0.5
    const Vec3 botLeft = pixel(r.image, 7, 24);    // u<0.5, v<0.5
    const Vec3 botRight = pixel(r.image, 24, 24);  // u>0.5, v<0.5

    // The checker reproduces regardless of the sampler's V orientation: the two
    // UV-diagonal samples match, the two anti-diagonal samples match, and the
    // diagonals carry different colors.
    REQUIRE(rgbEqual(topLeft, botRight));  // one diagonal
    REQUIRE(rgbEqual(topRight, botLeft));  // the other diagonal
    REQUIRE_FALSE(rgbEqual(topLeft, topRight));

    // Each sample is a pure checker texel (red or green), not a blended mix, and
    // the two diagonals carry opposite colors.
    const bool diagRed = isRed(topLeft) && isRed(botRight);
    const bool diagGreen = isGreen(topLeft) && isGreen(botRight);
    REQUIRE((diagRed || diagGreen));
    if (diagRed) {
        REQUIRE(isGreen(topRight));
        REQUIRE(isGreen(botLeft));
    } else {
        REQUIRE(isRed(topRight));
        REQUIRE(isRed(botLeft));
    }
}

TEST_CASE("texture color source falls back to vertex colors without a texture") {
    const Mesh low = makePlane(/*uv=*/true, /*color=*/false);
    const Mesh high = makePlane(/*uv=*/true, /*color=*/true, Vec3{0, 0, 1});  // blue vertex color

    bake::BakeParams p = params32();
    p.colorSource = bake::ColorSource{bake::ColorSource::Texture, /*texture=*/nullptr};
    const bake::BakeResult r = bake::bake(low, high, bake::BakeMap::Color, p);

    const Vec3 c = pixel(r.image, 16, 16);
    REQUIRE(c.z == doctest::Approx(1.0f).epsilon(0.02));  // blue from vertex colors
    REQUIRE(c.x == doctest::Approx(0.0f).epsilon(0.02));
    REQUIRE(c.y == doctest::Approx(0.0f).epsilon(0.02));
}

TEST_CASE("texture color source falls back to vertex colors when the Target has no UVs") {
    const Mesh low = makePlane(/*uv=*/true, /*color=*/false);
    const Mesh high = makePlane(/*uv=*/false, /*color=*/true, Vec3{0, 0, 1});  // no Target UVs
    const imageio::LoadedImage tex = checker2x2();

    bake::BakeParams p = params32();
    p.colorSource = bake::ColorSource{bake::ColorSource::Texture, &tex};
    const bake::BakeResult r = bake::bake(low, high, bake::BakeMap::Color, p);

    const Vec3 c = pixel(r.image, 16, 16);
    REQUIRE(c.z == doctest::Approx(1.0f).epsilon(0.02));  // fell back to vertex colors
}
