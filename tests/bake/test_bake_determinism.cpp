// Guards the build rule the bake pixel checksums depend on (cmake/FloatingPoint.cmake).
//
// The checksums in test_field_bake.cpp compare raw IEEE-754 bits, so they only
// hold while the toolchain rounds the bake's arithmetic the way they were
// captured. The one thing that silently changes that rounding is FMA
// contraction, and whether a compiler contracts is a property of the target ISA
// rather than of this source: aarch64 (iPad, Android, Apple silicon) has FMA in
// its baseline and contracts at -O2 and above, while a stock x86-64 build has
// no FMA instruction at all and therefore cannot. A checksum failure on one
// architecture and a green suite on the other is the symptom; these cases name
// the cause, so the next person does not go looking for it in the shading code.
#include <doctest.h>

#include <cstddef>
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

// Reads a float back through memory so the next expression cannot reuse the
// unrounded product the compiler still has in a register.
float opaque(float v) {
    volatile float slot = v;
    return slot;
}

// A quad in the z = 0 plane carrying its positions as UVs, and a Target whose
// vertices sit EXACTLY on it. Every cage ray then hits a triangle it is
// coplanar-adjacent to, which is where an inside test decided by the sign of a
// difference of products is most easily flipped.
Mesh planeWithUv(float z) {
    const std::vector<Vec3> p = {{0, 0, z}, {1, 0, z}, {1, 1, z}, {0, 1, z}};
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

}  // namespace

TEST_CASE("the build does not fuse multiply-add, so the bake goldens are portable") {
    // `x * x - round(x * x)` is zero when both products round, and the residual
    // of the single rounding when the expression contracts into fma(x, x, -p).
    // Separate volatile reads keep the compiler from proving the two products
    // equal and folding the subtraction away.
    const float x = opaque(1.0f + 0x1p-23f);
    const float y = opaque(1.0f + 0x1p-23f);
    const float rounded = opaque(x * y);
    const float residual = opaque(x) * opaque(y) - rounded;

    INFO("residual "
         << residual
         << " != 0 means this translation unit was compiled with FMA contraction "
            "enabled. Every dot, cross and difference of products in the bake then "
            "rounds once instead of twice and the pixel checksums in "
            "test_field_bake.cpp move. Configure with cmake/FloatingPoint.cmake's "
            "-ffp-contract=off (this is what -march=native strips off a stock x86-64 "
            "build, and what aarch64 does by default).");
    CHECK(residual == 0.0f);
}

TEST_CASE("a difference of products cancels exactly, so a coplanar bake keeps every texel") {
    // The rasterizer's barycentric denominator (d00 * d11 - d01 * d01) and the
    // ray/triangle determinant are both differences of products. Contracted,
    // only one side is rounded, so an exactly degenerate pair no longer cancels
    // to zero and the sign it produces is arbitrary — which is how a real bake
    // silently lost covered texels on arm64 while x86-64 CI stayed green.
    const float d00 = opaque(0.1f);
    const float d11 = opaque(0.3f);
    CHECK(opaque(d00) * opaque(d11) - opaque(d00) * opaque(d11) == 0.0f);

    // The end-to-end consequence: coverage is a coverage COUNT, not a
    // tolerance, so it pins the inside test's verdict rather than its arithmetic.
    const Mesh low = planeWithUv(0.0f);
    const Mesh high = planeWithUv(0.0f);
    bake::BakeParams params;
    params.width = 16;
    params.height = 16;
    params.cageDistance = 0.1f;

    const bake::BakeResult r = bake::bake(low, high, bake::BakeMap::Position, params);
    // A full-UV quad covers all 256 texels, and the 16 texels the two
    // fan-triangles share along the diagonal are rasterized by both — so the
    // count is an inside-test VERDICT count, which is precisely what a
    // contracted difference of products perturbs.
    CHECK(r.texelsCovered == 272);
    REQUIRE(r.image.pixels.size() == 16 * 16 * 3);

    // Every covered texel projected onto the coplanar Target, so the baked
    // position is the texel's own sample point: z is exactly 0 everywhere, with
    // no missed ray writing the untouched neutral padding.
    std::size_t exactZ = 0;
    for (int py = 0; py < params.height; ++py) {
        for (int px = 0; px < params.width; ++px) {
            if (r.image.at(px, py, 2) == 0.0f) {
                ++exactZ;
            }
        }
    }
    CHECK(exactZ == 256);
}
