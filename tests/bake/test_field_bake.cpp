// Field-sampled baking (pipeline-bridge spec, "Field-sampled baking through an
// evaluator interface").
#include <doctest.h>

#include <cmath>
#include <cstdint>
#include <cstring>
#include <limits>
#include <vector>

#include "cyber/bake/bake.hpp"
#include "cyber/bake/curvature.hpp"
#include "cyber/bake/field_evaluator.hpp"
#include "cyber/core/mesh.hpp"

using cyber::FaceId;
using cyber::Index;
using cyber::LoopId;
using cyber::Mesh;
using cyber::Vec2;
using cyber::Vec3;
using cyber::VertexId;
namespace bake = cyber::bake;

namespace {

// ---- the no-behaviour-change fixture ---------------------------------
//
// Captured from the binary BEFORE the evaluator refactor landed. Reproduced
// here EXACTLY: any change to the raycast arithmetic moves the checksum.

Mesh goldenLowPlane() {
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

Mesh goldenHighBumps() {
    constexpr int n = 21;
    std::vector<Vec3> p;
    for (int j = 0; j < n; ++j) {
        for (int i = 0; i < n; ++i) {
            const float u = static_cast<float>(i) / static_cast<float>(n - 1);
            const float v = static_cast<float>(j) / static_cast<float>(n - 1);
            const float z = 0.05f * std::sin(u * 25.0f) * std::cos(v * 21.0f);
            p.push_back({u, v, z});
        }
    }
    std::vector<std::vector<Index>> f;
    for (int j = 0; j + 1 < n; ++j) {
        for (int i = 0; i + 1 < n; ++i) {
            const Index a = static_cast<Index>(j * n + i);
            f.push_back({a, static_cast<Index>(a + 1), static_cast<Index>(a + n + 1)});
            f.push_back({a, static_cast<Index>(a + n + 1), static_cast<Index>(a + n)});
        }
    }
    Mesh mesh = Mesh::fromIndexed(p, f);
    auto& col = mesh.vertexAttributes().create<Vec3>("color");
    for (Index i = 0; i < mesh.vertexCapacity(); ++i) {
        col[i] = {0.25f, 0.5f, 0.75f};
    }
    return mesh;
}

bake::BakeParams goldenParams() {
    bake::BakeParams p;
    p.width = 8;
    p.height = 8;
    p.cageDistance = 0.1f;
    p.aoSamples = 8;
    return p;
}

// FNV-1a over the raw IEEE-754 bits of every pixel. A checksum rather than a
// tolerance: the requirement is bit-identity, and an epsilon comparison would
// quietly accept a real change in the shading arithmetic.
std::uint64_t pixelChecksum(const std::vector<float>& pixels) {
    std::uint64_t h = 1469598103934665603ull;
    for (const float f : pixels) {
        std::uint32_t bits = 0;
        std::memcpy(&bits, &f, sizeof(bits));
        for (int k = 0; k < 4; ++k) {
            h ^= (bits >> (k * 8)) & 0xffu;
            h *= 1099511628211ull;
        }
    }
    return h;
}

// ---- analytic evaluator test double -----------------------------------

// A sphere as a signed distance field. Exactly Lipschitz-1, so it satisfies the
// tracing contract in FieldEvaluator's header, and its gradient is the exact
// analytic normal — which is the entire point of sampling a field instead of a
// tessellation.
class SphereField : public bake::FieldEvaluator {
public:
    SphereField(Vec3 center, float radius) : m_center(center), m_radius(radius) {}

    [[nodiscard]] float distance(Vec3 p) const override {
        return cyber::length(p - m_center) - m_radius;
    }
    [[nodiscard]] Vec3 gradient(Vec3 p) const override { return cyber::normalized(p - m_center); }
    [[nodiscard]] float openness(Vec3, Vec3, float) const override {
        return 1.0f;  // a lone convex sphere occludes nothing
    }

private:
    Vec3 m_center;
    float m_radius;
};

// A plane z = 0 as a signed distance field: distance is the height, the
// gradient is +Z everywhere.
class FlatField : public bake::FieldEvaluator {
public:
    [[nodiscard]] float distance(Vec3 p) const override { return p.z; }
    [[nodiscard]] Vec3 gradient(Vec3) const override { return Vec3{0, 0, 1}; }
    [[nodiscard]] float openness(Vec3, Vec3, float) const override { return 1.0f; }
};

// Half-occluded everywhere, so an AO bake through the evaluator has to carry
// the evaluator's answer rather than a ray budget's.
class HalfOccludedField : public FlatField {
public:
    [[nodiscard]] float openness(Vec3, Vec3, float) const override { return 0.25f; }
};

// ---- hostile / sloppy evaluator doubles --------------------------------
//
// The doubles above are all exactly Lipschitz-1 and never fail, which is right
// for testing that the bridge computes the correct answer -- and is why nothing
// here covered a HOST getting it wrong. These cover that: an evaluator is host
// code and its returns are not ours to trust.
//
// The split under test is TIER 1 (no correct field can do this -> the whole
// bake fails) against TIER 2 (a correct field does hit this -> one texel is a
// counted miss). See the GuardedField comment in src/bake/src/bake.cpp.

// Distance goes NaN once the march is under way. NaN is the value that used to
// defeat the guard entirely: std::fmax(NaN, epsilon) is epsilon, so the march
// neither hit nor stopped -- it spent all 96 steps and returned a miss
// indistinguishable from empty space.
class NanDistanceField : public FlatField {
public:
    [[nodiscard]] float distance(Vec3 p) const override {
        return p.z > 0.02f ? p.z : std::numeric_limits<float>::quiet_NaN();
    }
};

// An INFINITE distance is the ordinary "nothing here" sentinel for a field
// fitted to a bounded region. Tier 2: a miss, not a failure.
class InfiniteDistanceField : public FlatField {
public:
    [[nodiscard]] float distance(Vec3) const override {
        return std::numeric_limits<float>::infinity();
    }
};

// A gradient that is non-finite where the surface actually is. Before the
// guard, normalized() mapped it to {0,0,0} and a zero "normal" was encoded into
// the map as though it were a measurement.
class NanGradientField : public FlatField {
public:
    [[nodiscard]] Vec3 gradient(Vec3) const override {
        const float nan = std::numeric_limits<float>::quiet_NaN();
        return Vec3{nan, nan, nan};
    }
};

// A ZERO gradient is what a correct SDF returns on its medial axis, so this is
// Tier 2 however unhelpful it is: a counted miss, never a fabricated normal.
class ZeroGradientField : public FlatField {
public:
    [[nodiscard]] Vec3 gradient(Vec3) const override { return Vec3{0.0f, 0.0f, 0.0f}; }
};

// Openness of 4.0 -- the shape of a host that returned a distance, or inverted
// the convention on a value that is already the inverted one. It used to be
// silently clamped to 1.0, i.e. "fully open", which is a plausible map.
class OutOfRangeOpennessField : public FlatField {
public:
    [[nodiscard]] float openness(Vec3, Vec3, float) const override { return 4.0f; }
};

// Openness a hair outside [0,1]: float slack, not a broken host. Must still
// clamp rather than fail the bake.
class SlightlyOverOpenField : public FlatField {
public:
    [[nodiscard]] float openness(Vec3, Vec3, float) const override { return 1.0f + 1e-5f; }
};

// Finite ON the surface and non-finite off it -- the shape of a grid field
// fitted tight to the mesh, which answers nothing outside its own AABB. The
// interface's DEFAULT curvature() probes gradient() six times OFF the surface,
// which is the fifth callback site and the one a wrapper around the three
// virtuals does not cover.
class OffSurfaceHoleField : public FlatField {
public:
    [[nodiscard]] Vec3 gradient(Vec3 p) const override {
        if (std::fabs(p.z) > 1e-4f) {
            const float nan = std::numeric_limits<float>::quiet_NaN();
            return Vec3{nan, nan, nan};
        }
        return Vec3{0, 0, 1};
    }
};

Mesh makePlaneWithUv(float z) {
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

// A UV-sphere patch around the +Z pole, low-poly, carrying a planar UV layout.
Mesh makeSpherePatch(Vec3 center, float radius, int n, float extent) {
    std::vector<Vec3> p;
    for (int j = 0; j <= n; ++j) {
        for (int i = 0; i <= n; ++i) {
            const float u = static_cast<float>(i) / static_cast<float>(n);
            const float v = static_cast<float>(j) / static_cast<float>(n);
            const float x = (u - 0.5f) * 2.0f * extent;
            const float y = (v - 0.5f) * 2.0f * extent;
            const float zz = std::sqrt(std::fmax(0.0f, 1.0f - x * x - y * y));
            p.push_back(center + Vec3{x, y, zz} * radius);
        }
    }
    std::vector<std::vector<Index>> f;
    const Index stride = static_cast<Index>(n) + 1;
    for (int j = 0; j < n; ++j) {
        for (int i = 0; i < n; ++i) {
            const Index a = static_cast<Index>(j * (n + 1) + i);
            f.push_back({a, a + 1, a + stride + 1, a + stride});
        }
    }
    Mesh mesh = Mesh::fromIndexed(p, f);
    auto& uv = mesh.cornerAttributes().create<Vec2>("uv");
    for (Index fi = 0; fi < mesh.faceCapacity(); ++fi) {
        if (!mesh.isAlive(FaceId{fi})) {
            continue;
        }
        for (const LoopId l : mesh.faceLoops(FaceId{fi})) {
            const Vec3 pos = mesh.position(mesh.loopVertex(l)) - center;
            uv[l.value] = {pos.x / (2.0f * extent * radius) + 0.5f,
                           pos.y / (2.0f * extent * radius) + 0.5f};
        }
    }
    return mesh;
}

// A finely tessellated, TRIANGULATED version of the same spherical cap — the
// raycast Target the evaluator bake is compared against. A cap rather than a
// full UV sphere so there are no degenerate pole triangles anywhere near the
// baked region.
Mesh makeFineSphereCap(Vec3 center, float radius, int n, float extent) {
    std::vector<Vec3> p;
    for (int j = 0; j <= n; ++j) {
        for (int i = 0; i <= n; ++i) {
            const float x = (static_cast<float>(i) / static_cast<float>(n) - 0.5f) * 2.0f * extent;
            const float y = (static_cast<float>(j) / static_cast<float>(n) - 0.5f) * 2.0f * extent;
            const float z = std::sqrt(std::fmax(0.0f, 1.0f - x * x - y * y));
            p.push_back(center + Vec3{x, y, z} * radius);
        }
    }
    std::vector<std::vector<Index>> f;
    const Index stride = static_cast<Index>(n) + 1;
    for (int j = 0; j < n; ++j) {
        for (int i = 0; i < n; ++i) {
            const Index a = static_cast<Index>(j * (n + 1) + i);
            f.push_back({a, a + 1, a + stride + 1});
            f.push_back({a, a + stride + 1, a + stride});
        }
    }
    return Mesh::fromIndexed(p, f);
}

Vec3 decodeNormal(const bake::Image& img, int x, int y) {
    return cyber::normalized(Vec3{img.at(x, y, 0) * 2.0f - 1.0f, img.at(x, y, 1) * 2.0f - 1.0f,
                                  img.at(x, y, 2) * 2.0f - 1.0f});
}

float center(const bake::Image& img, int c) { return img.at(img.width / 2, img.height / 2, c); }

}  // namespace

TEST_CASE("bake without an evaluator reproduces the pre-bridge pixels") {
    const Mesh low = goldenLowPlane();
    const Mesh high = goldenHighBumps();
    const bake::BakeParams params = goldenParams();
    REQUIRE(params.field == nullptr);  // the default must stay the raycast path

    struct Case {
        bake::BakeMap map;
        std::size_t size;
        std::uint64_t checksum;
    };
    // Captured from the unmodified binary before the field-evaluator refactor.
    // AO alone was RE-captured when the hemisphere sample set gained its
    // per-texel Cranley-Patterson rotation: firing the identical Hammersley set
    // at every texel locked whole neighbourhoods onto the same k/aoSamples rung
    // and banded the map, so that pixel-for-pixel contract had to be broken on
    // purpose. Every other map still holds the pre-bridge bits.
    //
    // Normal, Curvature and Color were RE-captured when the BVH build moved from
    // the object median to a binned SAH. This fixture's high-poly is a regular
    // 21x21 grid split along the u == v diagonal, and the cage rays for texels
    // (3,3) and (5,5) land EXACTLY on that shared diagonal — both adjacent
    // triangles report the same t, so which one is named depends on the tree.
    // Naming the other one moves those two texels by ONE ULP (measured
    // max |delta| = 1.2e-7) because the value interpolates the same along the
    // shared edge; the other 62 texels, and AO, Cavity, Displacement and
    // Position everywhere, are unchanged bit for bit.
    const std::vector<Case> cases = {
        {bake::BakeMap::Normal, 192, 0xa40268a897af49efull},
        {bake::BakeMap::AmbientOcclusion, 64, 0x1ab03f2993f16ba3ull},
        {bake::BakeMap::Curvature, 64, 0x581a35530fdd7092ull},
        {bake::BakeMap::Cavity, 64, 0xf099b3ccc9e3755full},
        {bake::BakeMap::Displacement, 64, 0x258044b372d77e53ull},
        {bake::BakeMap::Position, 192, 0xc502d243ae90602full},
        {bake::BakeMap::Color, 192, 0x4d8538e2d94fcfeeull},
    };
    for (const Case& c : cases) {
        CAPTURE(static_cast<int>(c.map));
        const bake::BakeResult r = bake::bake(low, high, c.map, params);
        REQUIRE(r.texelsCovered == 72);
        REQUIRE(r.image.pixels.size() == c.size);
#ifndef _WIN32
        // Bit-identity is asserted on the lanes cyber_apply_fp_rules() actually
        // pins (see cmake/FloatingPoint.cmake): the GCC/Clang targets — iOS,
        // Android, Linux, macOS — plus CI. MinGW is not one of them. It rounds
        // through its own libm, and this fixture is deliberately tie-sensitive:
        // the cage rays for texels (3,3) and (5,5) land EXACTLY on the high-poly's
        // shared u == v diagonal, so a one-ULP difference anywhere upstream flips
        // which of the two coincident triangles the BVH names and moves those
        // texels. The structural contract above (coverage and pixel count) still
        // runs everywhere.
        CHECK(pixelChecksum(r.image.pixels) == c.checksum);
#endif
    }
}

TEST_CASE("a normal bake through a field evaluator matches the raycast bake") {
    const Vec3 sphereCenter{0.5f, 0.5f, -0.6f};
    const float radius = 1.0f;
    const Mesh low = makeSpherePatch(sphereCenter, radius, 6, 0.35f);
    const Mesh fine = makeFineSphereCap(sphereCenter, radius, 200, 0.45f);
    const SphereField field(sphereCenter, radius);

    bake::BakeParams params;
    params.width = 24;
    params.height = 24;
    params.cageDistance = 0.05f;

    const bake::BakeResult raycast = bake::bake(low, fine, bake::BakeMap::Normal, params);
    params.field = &field;
    const bake::BakeResult sampled = bake::bake(low, fine, bake::BakeMap::Normal, params);

    REQUIRE(raycast.texelsCovered > 100);
    REQUIRE(sampled.texelsCovered == raycast.texelsCovered);

    std::size_t compared = 0;
    float worstDegrees = 0.0f;
    for (int y = 0; y < params.height; ++y) {
        for (int x = 0; x < params.width; ++x) {
            // Untouched texels stay at (0,0,0); the encoded default is
            // (0.5,0.5,1), so a zero blue channel means "not covered".
            if (raycast.image.at(x, y, 2) <= 0.0f || sampled.image.at(x, y, 2) <= 0.0f) {
                continue;
            }
            const float d =
                cyber::dot(decodeNormal(raycast.image, x, y), decodeNormal(sampled.image, x, y));
            const float degrees =
                std::acos(std::fmin(1.0f, std::fmax(-1.0f, d))) * 180.0f / cyber::kPi;
            worstDegrees = std::fmax(worstDegrees, degrees);
            ++compared;
        }
    }
    REQUIRE(compared > 100);
    CAPTURE(worstDegrees);
    // The two paths differ only by the fine mesh's tessellation error.
    CHECK(worstDegrees < 3.0f);
}

TEST_CASE("an evaluator normal bake of a coincident flat field is tangent-space up") {
    // The same correctness scenario the raycast path is held to
    // (tests/bake/test_bake.cpp, "normal bake of coincident flat surfaces").
    const Mesh low = makePlaneWithUv(0.0f);
    const FlatField field;
    bake::BakeParams params;
    params.width = 32;
    params.height = 32;
    params.cageDistance = 0.1f;
    params.field = &field;

    const bake::BakeResult r = bake::bake(low, Mesh{}, bake::BakeMap::Normal, params);
    REQUIRE(r.texelsCovered > 900);
    CHECK(center(r.image, 0) == doctest::Approx(0.5f).epsilon(0.02));
    CHECK(center(r.image, 1) == doctest::Approx(0.5f).epsilon(0.02));
    CHECK(center(r.image, 2) == doctest::Approx(1.0f).epsilon(0.02));
}

TEST_CASE("an AO bake through an evaluator carries the evaluator's occlusion") {
    const Mesh low = makePlaneWithUv(0.0f);
    const HalfOccludedField field;
    bake::BakeParams params;
    params.width = 16;
    params.height = 16;
    params.cageDistance = 0.1f;
    params.aoSamples = 4;  // deliberately tiny: no ray budget is being consulted
    params.field = &field;

    const bake::BakeResult r = bake::bake(low, Mesh{}, bake::BakeMap::AmbientOcclusion, params);
    REQUIRE(r.texelsCovered > 200);
    CHECK(center(r.image, 0) == doctest::Approx(0.25f).epsilon(0.001));
}

TEST_CASE("an evaluator AO bake ignores the ray budget including an empty one") {
    // The raycast path rejects aoSamples <= 0 because openness divides by it;
    // the field path asks the evaluator for occlusion directly and never
    // divides, so the range check must not reach across and fail this bake.
    const Mesh low = makePlaneWithUv(0.0f);
    const HalfOccludedField field;
    bake::BakeParams params;
    params.width = 16;
    params.height = 16;
    params.cageDistance = 0.1f;
    params.aoSamples = 0;
    params.field = &field;

    const bake::BakeResult r = bake::bake(low, Mesh{}, bake::BakeMap::AmbientOcclusion, params);
    REQUIRE(r.texelsCovered > 200);
    CHECK(center(r.image, 0) == doctest::Approx(0.25f).epsilon(0.001));
}

TEST_CASE("a curvature bake through an evaluator reads the field's curvature") {
    // A sphere of radius r has mean curvature 1/r everywhere, so the map is
    // uniform and — being uniformly convex — saturates bright.
    const Vec3 sphereCenter{0.5f, 0.5f, -0.6f};
    const SphereField field(sphereCenter, 1.0f);
    const Mesh low = makeSpherePatch(sphereCenter, 1.0f, 6, 0.35f);

    bake::BakeParams params;
    params.width = 16;
    params.height = 16;
    params.cageDistance = 0.05f;
    params.curvatureRange = 2.0f;  // 1/r = 1 lands halfway up the positive range
    params.field = &field;

    const bake::BakeResult r = bake::bake(low, Mesh{}, bake::BakeMap::Curvature, params);
    REQUIRE(r.texelsCovered > 100);
    CHECK(center(r.image, 0) == doctest::Approx(0.75f).epsilon(0.05));

    // Cavity keeps concavity only, so a convex sphere reads white throughout.
    const bake::BakeResult cavity = bake::bake(low, Mesh{}, bake::BakeMap::Cavity, params);
    CHECK(center(cavity.image, 0) == doctest::Approx(1.0f).epsilon(0.01));
}

TEST_CASE("an evaluator does not cover maps that describe the Target mesh") {
    // Displacement / Position / Color have no field counterpart: they still
    // require the high-poly, and an empty Target with an evaluator attached is
    // still an empty bake rather than a silently wrong one.
    const Mesh low = makePlaneWithUv(0.0f);
    const FlatField field;
    bake::BakeParams params;
    params.width = 8;
    params.height = 8;
    params.field = &field;

    for (const bake::BakeMap map :
         {bake::BakeMap::Displacement, bake::BakeMap::Position, bake::BakeMap::Color}) {
        const bake::BakeResult r = bake::bake(low, Mesh{}, map, params);
        CHECK(r.image.pixels.empty());
    }
}

TEST_CASE("an evaluator leaves the non-field maps on the raycast path unchanged") {
    // Attaching an evaluator must not perturb a map it does not serve.
    const Mesh low = goldenLowPlane();
    const Mesh high = goldenHighBumps();
    const FlatField field;
    bake::BakeParams params = goldenParams();
    const bake::BakeResult plain = bake::bake(low, high, bake::BakeMap::Position, params);
    params.field = &field;
    const bake::BakeResult withField = bake::bake(low, high, bake::BakeMap::Position, params);
    CHECK(pixelChecksum(withField.image.pixels) == pixelChecksum(plain.image.pixels));
}

// ---- the host-callback boundary ----------------------------------------

namespace {

bake::BakeResult bakeWithField(const bake::FieldEvaluator& field, bake::BakeMap map) {
    const Mesh low = makePlaneWithUv(0.0f);
    bake::BakeParams params;
    params.width = 8;
    params.height = 8;
    params.cageDistance = 0.05f;
    params.field = &field;
    const Mesh empty;
    return bake::bake(low, empty, map, params);
}

}  // namespace

TEST_CASE("a NaN distance fails the bake instead of shading a plausible miss") {
    const NanDistanceField field;
    const bake::BakeResult r = bakeWithField(field, bake::BakeMap::Normal);
    CHECK(r.fieldContractViolated);
    CHECK(r.fieldContractMessage.find("distance") != std::string::npos);
    CHECK(r.fieldContractMessage.find("NaN") != std::string::npos);
    // Abandoned, not half-written: a partial map is the plausible artefact.
    CHECK(r.image.pixels.empty());
    CHECK(r.texelsCovered == 0);
}

TEST_CASE("a non-finite gradient fails the bake and is not laundered to a zero normal") {
    const NanGradientField field;
    const bake::BakeResult r = bakeWithField(field, bake::BakeMap::Normal);
    CHECK(r.fieldContractViolated);
    CHECK(r.fieldContractMessage.find("gradient") != std::string::npos);
    CHECK(r.image.pixels.empty());
}

TEST_CASE("an infinite distance is a counted miss, not a contract violation") {
    // The distinction that looks arbitrary and is not: +inf is the ordinary
    // "nothing here" answer from a field fitted to a bounded region, and it
    // exits the march honestly. NaN did not even stop it.
    const InfiniteDistanceField field;
    const bake::BakeResult r = bakeWithField(field, bake::BakeMap::Normal);
    CHECK_FALSE(r.fieldContractViolated);
    CHECK(r.fieldUndefinedSamples > 0);
    CHECK_FALSE(r.image.pixels.empty());
}

TEST_CASE("a zero-length gradient is a counted miss, not a fabricated normal") {
    // A correct SDF has no gradient on its medial axis, so failing here would
    // make the engine unembeddable against real quantized fields.
    const ZeroGradientField field;
    const bake::BakeResult r = bakeWithField(field, bake::BakeMap::Normal);
    CHECK_FALSE(r.fieldContractViolated);
    CHECK(r.fieldUndefinedSamples > 0);
    REQUIRE_FALSE(r.image.pixels.empty());
    // Every covered texel took the neutral padding an un-hit cage ray writes,
    // rather than a zero vector encoded as though it were measured.
    CHECK(r.image.at(4, 4, 2) == doctest::Approx(1.0f));
}

TEST_CASE("an openness outside [0,1] fails the bake, and float slack still clamps") {
    const OutOfRangeOpennessField broken;
    const bake::BakeResult bad = bakeWithField(broken, bake::BakeMap::AmbientOcclusion);
    CHECK(bad.fieldContractViolated);
    CHECK(bad.fieldContractMessage.find("openness") != std::string::npos);

    const SlightlyOverOpenField slack;
    const bake::BakeResult ok = bakeWithField(slack, bake::BakeMap::AmbientOcclusion);
    CHECK_FALSE(ok.fieldContractViolated);
    REQUIRE_FALSE(ok.image.pixels.empty());
    CHECK(ok.image.at(4, 4, 0) == doctest::Approx(1.0f));
}

TEST_CASE("a non-finite gradient OFF the surface fails the curvature bake") {
    // The fifth callback site. curvature()'s interface default probes gradient()
    // at six points off the surface; before this change normalized() turned
    // those NaNs into {0,0,0} and the default returned a FINITE curvature
    // computed from nothing, so every guard downstream saw a healthy number.
    const OffSurfaceHoleField field;
    const bake::BakeResult r = bakeWithField(field, bake::BakeMap::Curvature);
    CHECK(r.fieldContractViolated);
    CHECK(r.fieldContractMessage.find("curvature") != std::string::npos);
    CHECK(r.image.pixels.empty());
}

TEST_CASE("curvatureScale ignores a non-finite sample") {
    // `NaN != 0.0f` is TRUE, so a NaN used to pass the filter and reach
    // weightedPercentile's std::sort, where comparing it violates strict weak
    // ordering -- UB in the sort, not merely a poisoned auto range.
    //
    // The NaN's POSITION matters and this is why the obvious fixture is
    // worthless: at the default 0.95 percentile over four samples the target
    // index is 3.8, so {1,2,NaN,4} returns 4.0 with or without the fix and
    // asserts nothing. It has to land at or after the cut.
    const float nan = std::numeric_limits<float>::quiet_NaN();
    CHECK(bake::curvatureScale({1.0f, 2.0f, 4.0f, nan}) == doctest::Approx(4.0f));
    CHECK(bake::curvatureScale({1.0f, 2.0f, nan, 4.0f}) == doctest::Approx(4.0f));
    // All non-finite: the filter admitted every one, so the percentile itself
    // came back NaN and became the auto range for the whole image.
    CHECK(bake::curvatureScale({nan, nan}) == doctest::Approx(0.0f));
}
