#include <doctest.h>

#include <algorithm>
#include <atomic>
#include <cmath>
#include <functional>
#include <limits>
#include <memory>
#include <mutex>
#include <string>
#include <string_view>
#include <vector>

#include "cyber/accel/backend.hpp"
#include "cyber/bake/bake.hpp"
#include "cyber/core/mesh.hpp"
#include "cyber/core/progress.hpp"

using cyber::CancelToken;
using cyber::FaceId;
using cyber::Index;
using cyber::LoopId;
using cyber::Mesh;
using cyber::ProgressSink;
using cyber::Vec2;
using cyber::Vec3;
namespace accel = cyber::accel;
namespace bake = cyber::bake;

// The CyberTexel mesh-map set (surface-baking spec: "Object-space normal and
// position maps", "Bent normal and thickness maps", "Baked maps record their
// encoding basis").
namespace {

// Quad in the z = `z` plane spanning [x0,x1] x [y0,y1], with UVs that map the
// quad onto the whole [0,1]^2 layout however far from the origin it sits. That
// matters: a position map must be tested somewhere its model-unit coordinates
// are NOT already in [0,1], or an unencoded map and a bbox-encoded one look the
// same.
Mesh planeWithUv(float z, float x0, float x1, float y0, float y1) {
    const std::vector<Vec3> p = {{x0, y0, z}, {x1, y0, z}, {x1, y1, z}, {x0, y1, z}};
    const std::vector<std::vector<Index>> f = {{0, 1, 2, 3}};
    Mesh mesh = Mesh::fromIndexed(p, f);
    auto& uv = mesh.cornerAttributes().create<Vec2>("uv");
    for (Index fi = 0; fi < mesh.faceCapacity(); ++fi) {
        if (!mesh.isAlive(FaceId{fi})) {
            continue;
        }
        for (const LoopId l : mesh.faceLoops(FaceId{fi})) {
            const Vec3 pos = mesh.position(mesh.loopVertex(l));
            uv[l.value] = {(pos.x - x0) / (x1 - x0), (pos.y - y0) / (y1 - y0)};
        }
    }
    return mesh;
}

// A quad whose UVs cover only the lower-left quarter of the layout, so three
// quarters of the image is padding.
Mesh quarterChartPlane() {
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
            uv[l.value] = {pos.x * 0.5f, pos.y * 0.5f};
        }
    }
    return mesh;
}

// The same quad with no UVs: a Target never needs them.
Mesh plane(float z, float x0, float x1, float y0, float y1) {
    const std::vector<Vec3> p = {{x0, y0, z}, {x1, y0, z}, {x1, y1, z}, {x0, y1, z}};
    const std::vector<std::vector<Index>> f = {{0, 1, 2, 3}};
    return Mesh::fromIndexed(p, f);
}

// The same quad wound the other way: its outward normal points down.
Mesh flippedPlane(float z, float x0, float x1, float y0, float y1) {
    const std::vector<Vec3> p = {{x0, y0, z}, {x1, y0, z}, {x1, y1, z}, {x0, y1, z}};
    const std::vector<std::vector<Index>> f = {{0, 3, 2, 1}};
    return Mesh::fromIndexed(p, f);
}

// Closed box with OUTWARD face normals (same winding as tests/core makeCube).
Mesh box(float x0, float x1, float y0, float y1, float z0, float z1) {
    const std::vector<Vec3> p = {{x0, y0, z0}, {x1, y0, z0}, {x1, y1, z0}, {x0, y1, z0},
                                 {x0, y0, z1}, {x1, y0, z1}, {x1, y1, z1}, {x0, y1, z1}};
    const std::vector<std::vector<Index>> f = {
        {0, 3, 2, 1}, {4, 5, 6, 7}, {0, 1, 5, 4}, {1, 2, 6, 5}, {2, 3, 7, 6}, {3, 0, 4, 7},
    };
    return Mesh::fromIndexed(p, f);
}

// Appends `other`'s faces to `mesh`, keeping their own vertices so the two
// surfaces do not share (and average away) their vertex normals.
void append(Mesh& mesh, const Mesh& other) {
    std::vector<cyber::VertexId> mapped(other.vertexCapacity(), cyber::VertexId{});
    for (Index vi = 0; vi < other.vertexCapacity(); ++vi) {
        const cyber::VertexId v{vi};
        if (other.isAlive(v)) {
            mapped[vi] = mesh.addVertex(other.position(v));
        }
    }
    for (Index fi = 0; fi < other.faceCapacity(); ++fi) {
        const FaceId f{fi};
        if (!other.isAlive(f)) {
            continue;
        }
        std::vector<cyber::VertexId> corners;
        for (const cyber::VertexId v : other.faceVertices(f)) {
            corners.push_back(mapped[v.value]);
        }
        mesh.addFace(corners);
    }
}

Vec3 texel(const bake::Image& img, float u, float v) {
    const int px =
        std::clamp(static_cast<int>(u * static_cast<float>(img.width)), 0, img.width - 1);
    const int py = std::clamp(static_cast<int>((1.0f - v) * static_cast<float>(img.height)), 0,
                              img.height - 1);
    return Vec3{img.at(px, py, 0), img.channels > 1 ? img.at(px, py, 1) : 0.0f,
                img.channels > 2 ? img.at(px, py, 2) : 0.0f};
}

bake::BakeParams params64() {
    bake::BakeParams p;
    p.width = 64;
    p.height = 64;
    p.cageDistance = 0.05f;
    p.aoSamples = 64;
    p.aoRadius = 1.0f;
    return p;
}

// Backend that records the ranges a bake hands to parallelFor and delegates to
// the CPU reference, so a test can see WHERE the parallelism sits.
class RecordingBackend final : public accel::IBackend {
public:
    explicit RecordingBackend(std::shared_ptr<accel::IBackend> inner) : m_inner(std::move(inner)) {}

    [[nodiscard]] accel::BackendKind kind() const override { return m_inner->kind(); }
    [[nodiscard]] std::string deviceName() const override { return m_inner->deviceName(); }

    void parallelFor(std::size_t begin, std::size_t end,
                     const std::function<void(std::size_t, std::size_t)>& fn) override {
        {
            const std::lock_guard<std::mutex> lock(m_mutex);
            m_ranges.push_back(end - begin);
        }
        m_inner->parallelFor(begin, end, fn);
    }

    [[nodiscard]] std::vector<std::size_t> ranges() const {
        const std::lock_guard<std::mutex> lock(m_mutex);
        return m_ranges;
    }

private:
    std::shared_ptr<accel::IBackend> m_inner;
    mutable std::mutex m_mutex;
    std::vector<std::size_t> m_ranges;
};

class ScopedBackend {
public:
    explicit ScopedBackend(std::shared_ptr<accel::IBackend> backend)
        : m_previous(accel::defaultBackend()) {
        accel::setDefaultBackend(std::move(backend));
    }
    ~ScopedBackend() { accel::setDefaultBackend(m_previous); }
    ScopedBackend(const ScopedBackend&) = delete;
    ScopedBackend& operator=(const ScopedBackend&) = delete;

private:
    std::shared_ptr<accel::IBackend> m_previous;
};

}  // namespace

// ---- object-space normal --------------------------------------------------

TEST_CASE("object-space normal encodes the Target normal in the requested up axis") {
    const Mesh low = planeWithUv(0, 0, 1, 0, 1);
    const Mesh high = plane(0, -1, 2, -1, 2);

    bake::BakeParams p = params64();
    const bake::BakeResult yUp = bake::bake(low, high, bake::BakeMap::ObjectNormal, p);
    REQUIRE_FALSE(yUp.image.pixels.empty());
    REQUIRE(yUp.image.channels == 3);
    // The Target normal is +z, which encodes to (0.5, 0.5, 1).
    const Vec3 y = texel(yUp.image, 0.5f, 0.5f);
    CHECK(y.x == doctest::Approx(0.5f).epsilon(0.01));
    CHECK(y.y == doctest::Approx(0.5f).epsilon(0.01));
    CHECK(y.z == doctest::Approx(1.0f).epsilon(0.01));

    // z-up re-expresses (x, y, z) as (x, -z, y), so +z becomes -y: (0.5, 0, 0.5).
    p.upAxis = bake::UpAxis::ZUp;
    const bake::BakeResult zUp = bake::bake(low, high, bake::BakeMap::ObjectNormal, p);
    const Vec3 z = texel(zUp.image, 0.5f, 0.5f);
    CHECK(z.x == doctest::Approx(0.5f).epsilon(0.01));
    CHECK(z.y == doctest::Approx(0.0f).epsilon(0.01));
    CHECK(z.z == doctest::Approx(0.5f).epsilon(0.01));
    CHECK(yUp.image.pixels != zUp.image.pixels);  // applied, not merely declared

    CHECK(yUp.encoding.basis == bake::EncodingBasis::ObjectNormal);
    CHECK(yUp.encoding.upAxis == bake::UpAxis::YUp);
    CHECK(zUp.encoding.upAxis == bake::UpAxis::ZUp);
}

// ---- object-space position ------------------------------------------------

TEST_CASE("object-space position spans the recorded bounds and decodes back") {
    // Far from the origin and 2 units wide, so an unencoded map could not pass.
    const Mesh low = planeWithUv(0, 2, 4, 2, 4);
    const Mesh high = plane(0, 2, 4, 2, 4);
    const bake::BakeResult r = bake::bake(low, high, bake::BakeMap::ObjectPosition, params64());
    REQUIRE_FALSE(r.image.pixels.empty());

    REQUIRE(r.encoding.basis == bake::EncodingBasis::ObjectBounds);
    CHECK(r.encoding.boundsMin.x == doctest::Approx(2.0f));
    CHECK(r.encoding.boundsMax.x == doctest::Approx(4.0f));
    CHECK(r.encoding.boundsMin.z == doctest::Approx(0.0f));
    CHECK(r.encoding.boundsMax.z == doctest::Approx(0.0f));

    // Every covered texel is inside [0,1] on every channel.
    for (int py = 0; py < r.image.height; ++py) {
        for (int px = 0; px < r.image.width; ++px) {
            for (int c = 0; c < 3; ++c) {
                const float v = r.image.at(px, py, c);
                REQUIRE(std::isfinite(v));
                REQUIRE(v >= 0.0f);
                REQUIRE(v <= 1.0f);
            }
        }
    }

    // Opposite ends of the layout differ along the axis they are separated on.
    CHECK(texel(r.image, 0.1f, 0.5f).x < texel(r.image, 0.9f, 0.5f).x - 0.5f);
    CHECK(texel(r.image, 0.5f, 0.1f).y < texel(r.image, 0.5f, 0.9f).y - 0.5f);

    // Decoding with the recorded basis recovers the model-unit coordinate. The
    // texel at UV (0.75, 0.25) sits at x = 2 + 0.75*2, y = 2 + 0.25*2.
    const Vec3 encoded = texel(r.image, 0.75f, 0.25f);
    const Vec3 span = r.encoding.boundsMax - r.encoding.boundsMin;
    const Vec3 decoded{r.encoding.boundsMin.x + encoded.x * span.x,
                       r.encoding.boundsMin.y + encoded.y * span.y,
                       r.encoding.boundsMin.z + encoded.z * span.z};
    CHECK(decoded.x == doctest::Approx(3.5f).epsilon(0.05));
    CHECK(decoded.y == doctest::Approx(2.5f).epsilon(0.05));
}

TEST_CASE("a zero-extent axis encodes to the midpoint, not to a NaN") {
    const Mesh low = planeWithUv(0, 0, 1, 0, 1);
    const Mesh high = plane(0, 0, 1, 0, 1);  // no extent on z at all
    const bake::BakeResult r = bake::bake(low, high, bake::BakeMap::ObjectPosition, params64());
    REQUIRE_FALSE(r.image.pixels.empty());
    CHECK(texel(r.image, 0.5f, 0.5f).z == doctest::Approx(0.5f));
    for (const float v : r.image.pixels) {
        REQUIRE(std::isfinite(v));
    }
}

TEST_CASE("the up axis reaches the position map too") {
    const Mesh low = planeWithUv(0, 0, 1, 0, 1);
    const Mesh high = plane(0, 0, 1, 0, 1);
    bake::BakeParams p = params64();
    p.upAxis = bake::UpAxis::ZUp;
    const bake::BakeResult r = bake::bake(low, high, bake::BakeMap::ObjectPosition, p);
    REQUIRE_FALSE(r.image.pixels.empty());
    // (x, y, z) -> (x, -z, y): the flat axis moves from z to y, and the bounds
    // are reported in the SAME convention so a consumer decodes without one.
    CHECK(r.encoding.boundsMin.y == doctest::Approx(0.0f));
    CHECK(r.encoding.boundsMax.y == doctest::Approx(0.0f));
    CHECK(r.encoding.boundsMax.z == doctest::Approx(1.0f));
    CHECK(texel(r.image, 0.5f, 0.5f).y == doctest::Approx(0.5f));
    CHECK(texel(r.image, 0.5f, 0.9f).z > 0.8f);
}

TEST_CASE("the world-space position map still writes model units") {
    // The redefinition this change refused: BakeMap::Position means the hit
    // point in model units, and a caller reading coordinates keeps getting them.
    const Mesh low = planeWithUv(0, 2, 4, 2, 4);
    const Mesh high = plane(0, 2, 4, 2, 4);
    const bake::BakeResult r = bake::bake(low, high, bake::BakeMap::Position, params64());
    REQUIRE_FALSE(r.image.pixels.empty());
    const Vec3 centre = texel(r.image, 0.5f, 0.5f);
    CHECK(centre.x == doctest::Approx(3.0f).epsilon(0.05));
    CHECK(centre.y == doctest::Approx(3.0f).epsilon(0.05));
    CHECK(r.encoding.basis == bake::EncodingBasis::None);
}

// ---- bent normal ----------------------------------------------------------

TEST_CASE("the bent normal leans away from an occluder") {
    // Flat low-poly; the Target adds a tall wall standing at x = 0.5.
    const Mesh low = planeWithUv(0, 0, 1, 0, 1);
    Mesh high = plane(0, -1, 2, -1, 2);
    {
        const std::vector<Vec3> p = {
            {0.5f, -1, 0}, {0.5f, 2, 0}, {0.5f, 2, 0.5f}, {0.5f, -1, 0.5f}};
        const std::vector<std::vector<Index>> f = {{0, 1, 2, 3}};
        append(high, Mesh::fromIndexed(p, f));
    }

    bake::BakeParams p = params64();
    p.bentNormalSpace = bake::NormalSpace::Object;
    const bake::BakeResult r = bake::bake(low, high, bake::BakeMap::BentNormal, p);
    REQUIRE_FALSE(r.image.pixels.empty());
    REQUIRE(r.encoding.basis == bake::EncodingBasis::ObjectNormal);

    // Right of the wall the open directions are +x, so the encoded red channel
    // rises above the 0.5 that a straight-up normal encodes to.
    CHECK(texel(r.image, 0.55f, 0.5f).x > 0.6f);
    // Left of the wall it leans the other way.
    CHECK(texel(r.image, 0.45f, 0.5f).x < 0.4f);
    // Far from the wall the hemisphere is nearly open, so the bent normal stays
    // close to the surface normal: (0, 0, 1) encoded.
    const Vec3 open = texel(r.image, 0.02f, 0.5f);
    CHECK(open.z > 0.9f);
    CHECK(std::fabs(open.x - 0.5f) < std::fabs(texel(r.image, 0.45f, 0.5f).x - 0.5f));
}

TEST_CASE("a fully enclosed texel falls back to the surface normal") {
    // A closed room: floor at z = 0 and a ceiling just above it, wide enough
    // that every one of the 64 cosine-weighted rays hits within the radius.
    const Mesh low = planeWithUv(0, 0, 1, 0, 1);
    Mesh high = plane(0, -2, 3, -2, 3);
    // The ceiling has to reach at least as far as the radius, because a
    // cosine-weighted hemisphere still fires near-grazing rays.
    append(high, flippedPlane(0.05f, -1200, 1200, -1200, 1200));

    bake::BakeParams p = params64();
    p.cageDistance = 0.01f;  // the cage must not start ON the ceiling
    p.aoRadius = 1000.0f;
    const bake::BakeResult occluded = bake::bake(low, high, bake::BakeMap::AmbientOcclusion, p);
    REQUIRE_FALSE(occluded.image.pixels.empty());
    REQUIRE(texel(occluded.image, 0.5f, 0.5f).x == doctest::Approx(0.0f));  // nothing got out

    const bake::BakeResult bent = bake::bake(low, high, bake::BakeMap::BentNormal, p);
    const Vec3 v = texel(bent.image, 0.5f, 0.5f);
    // Tangent space, so the surface normal is (0, 0, 1) -> (0.5, 0.5, 1). Not a
    // zero vector normalized into whatever the float arithmetic produced.
    CHECK(v.x == doctest::Approx(0.5f).epsilon(0.001));
    CHECK(v.y == doctest::Approx(0.5f).epsilon(0.001));
    CHECK(v.z == doctest::Approx(1.0f).epsilon(0.001));
}

TEST_CASE("the bent normal's frame is a parameter, and it is recorded") {
    const Mesh low = planeWithUv(0, 0, 1, 0, 1);
    Mesh high = plane(0, -1, 2, -1, 2);
    {
        const std::vector<Vec3> p = {
            {0.5f, -1, 0}, {0.5f, 2, 0}, {0.5f, 2, 0.5f}, {0.5f, -1, 0.5f}};
        const std::vector<std::vector<Index>> f = {{0, 1, 2, 3}};
        append(high, Mesh::fromIndexed(p, f));
    }

    bake::BakeParams p = params64();
    const bake::BakeResult tangent = bake::bake(low, high, bake::BakeMap::BentNormal, p);
    CHECK(tangent.encoding.basis == bake::EncodingBasis::TangentNormal);

    p.bentNormalSpace = bake::NormalSpace::Object;
    p.upAxis = bake::UpAxis::ZUp;
    const bake::BakeResult zUp = bake::bake(low, high, bake::BakeMap::BentNormal, p);
    CHECK(zUp.encoding.basis == bake::EncodingBasis::ObjectNormal);
    CHECK(zUp.encoding.upAxis == bake::UpAxis::ZUp);

    // This low-poly's tangent frame is the identity, so tangent space and y-up
    // object space coincide; z-up does not, and the channels permute.
    p.upAxis = bake::UpAxis::YUp;
    const bake::BakeResult yUp = bake::bake(low, high, bake::BakeMap::BentNormal, p);
    CHECK(texel(yUp.image, 0.55f, 0.5f).z == doctest::Approx(texel(tangent.image, 0.55f, 0.5f).z));
    CHECK(texel(zUp.image, 0.55f, 0.5f).y ==
          doctest::Approx(1.0f - texel(yUp.image, 0.55f, 0.5f).z).epsilon(0.001));
}

// ---- thickness ------------------------------------------------------------

TEST_CASE("thickness reads a solid and tracks its depth") {
    const Mesh low = planeWithUv(0, 0, 1, 0, 1);
    const Mesh thick = box(0, 1, 0, 1, -0.5f, 0);
    const Mesh thin = box(0, 1, 0, 1, -0.02f, 0);

    bake::BakeParams p = params64();
    p.aoRadius = 4.0f;
    const bake::BakeResult deep = bake::bake(low, thick, bake::BakeMap::Thickness, p);
    const bake::BakeResult shallow = bake::bake(low, thin, bake::BakeMap::Thickness, p);
    REQUIRE_FALSE(deep.image.pixels.empty());
    REQUIRE(deep.image.channels == 1);

    const float deepValue = texel(deep.image, 0.5f, 0.5f).x;
    const float shallowValue = texel(shallow.image, 0.5f, 0.5f).x;
    CHECK(deepValue > 0.2f);
    CHECK(shallowValue > 0.0f);
    CHECK(shallowValue < deepValue * 0.2f);  // a shallower solid reads thinner

    CHECK(deep.encoding.basis == bake::EncodingBasis::Distance);
    CHECK(deep.encoding.scale == doctest::Approx(2.0f));
}

TEST_CASE("a thin double-sided surface reads near zero rather than its thickness") {
    // The trap. With the cage applied the sample sits ON the surface and the
    // hemisphere is fired INTO it; a sheet with no interior has no back face in
    // the way, so the rays escape. Reading a miss as "maximally thick" would
    // paint every open sheet solid white.
    const Mesh low = planeWithUv(0, 0, 1, 0, 1);
    const Mesh sheet = plane(0, -1, 2, -1, 2);  // one quad: no volume at all

    bake::BakeParams p = params64();
    p.aoRadius = 4.0f;
    const bake::BakeResult r = bake::bake(low, sheet, bake::BakeMap::Thickness, p);
    REQUIRE_FALSE(r.image.pixels.empty());
    CHECK(texel(r.image, 0.5f, 0.5f).x == doctest::Approx(0.0f));

    // A plate meshed as two surfaces 2 mm apart is double-sided in the other
    // sense, and reads its own (near-zero) thickness rather than the radius.
    Mesh plate = plane(0, -1, 2, -1, 2);
    append(plate, flippedPlane(-0.002f, -1, 2, -1, 2));
    const bake::BakeResult plated = bake::bake(low, plate, bake::BakeMap::Thickness, p);
    const float value = texel(plated.image, 0.5f, 0.5f).x;
    CHECK(value > 0.0f);
    CHECK(value < 0.05f);
}

TEST_CASE("the thickness scale is a stated parameter, applied and recorded") {
    const Mesh low = planeWithUv(0, 0, 1, 0, 1);
    const Mesh solid = box(0, 1, 0, 1, -0.5f, 0);

    bake::BakeParams p = params64();
    p.aoRadius = 4.0f;
    p.thicknessScale = 1.0f;
    const bake::BakeResult one = bake::bake(low, solid, bake::BakeMap::Thickness, p);
    p.thicknessScale = 4.0f;
    const bake::BakeResult four = bake::bake(low, solid, bake::BakeMap::Thickness, p);
    REQUIRE_FALSE(one.image.pixels.empty());

    CHECK(texel(four.image, 0.5f, 0.5f).x ==
          doctest::Approx(4.0f * texel(one.image, 0.5f, 0.5f).x).epsilon(0.001));
    CHECK(one.encoding.scale == doctest::Approx(1.0f));
    CHECK(four.encoding.scale == doctest::Approx(4.0f));
}

// ---- shared with the AO baker --------------------------------------------

TEST_CASE("the new maps honour the cage, the ceiling, the UVs and their ranges") {
    const Mesh lowNoUv = plane(0, 0, 1, 0, 1);
    const Mesh low = planeWithUv(0, 0, 1, 0, 1);
    const Mesh high = box(0, 1, 0, 1, -0.5f, 0);
    const std::vector<bake::BakeMap> maps = {bake::BakeMap::ObjectNormal,
                                             bake::BakeMap::ObjectPosition,
                                             bake::BakeMap::BentNormal, bake::BakeMap::Thickness};

    for (const bake::BakeMap map : maps) {
        CAPTURE(static_cast<int>(map));
        // No UVs on the low-poly: nothing to rasterize into.
        CHECK(bake::bake(lowNoUv, high, map, params64()).image.pixels.empty());
        // No Target: nothing to project onto.
        CHECK(bake::bake(low, Mesh{}, map, params64()).image.pixels.empty());
        // Over the host's texel ceiling.
        bake::BakeParams capped = params64();
        capped.maxPixels = 100;
        CHECK(bake::bake(low, high, map, capped).image.pixels.empty());
        // A non-finite cage, which every map reads.
        bake::BakeParams badCage = params64();
        badCage.cageDistance = std::numeric_limits<float>::quiet_NaN();
        CHECK(bake::bake(low, high, map, badCage).image.pixels.empty());
    }

    // The ray-traced pair reads the AO budget, so a zero budget is refused for
    // them exactly as it is for AO -- and ignored by the rasterized pair, which
    // never divides by it.
    bake::BakeParams noBudget = params64();
    noBudget.aoSamples = 0;
    CHECK(bake::bake(low, high, bake::BakeMap::BentNormal, noBudget).image.pixels.empty());
    CHECK(bake::bake(low, high, bake::BakeMap::Thickness, noBudget).image.pixels.empty());
    CHECK_FALSE(bake::bake(low, high, bake::BakeMap::ObjectNormal, noBudget).image.pixels.empty());

    // The thickness scale, which only thickness reads.
    bake::BakeParams badScale = params64();
    badScale.thicknessScale = -1.0f;
    CHECK(bake::bake(low, high, bake::BakeMap::Thickness, badScale).image.pixels.empty());
    badScale.thicknessScale = std::numeric_limits<float>::infinity();
    CHECK(bake::bake(low, high, bake::BakeMap::Thickness, badScale).image.pixels.empty());
    CHECK_FALSE(bake::bake(low, high, bake::BakeMap::ObjectNormal, badScale).image.pixels.empty());
}

TEST_CASE("the cage decides which Target surface an object-space map speaks for") {
    const Mesh low = planeWithUv(0, 0, 1, 0, 1);
    // A Target far below the cage: the projection misses and the map falls back
    // to the low-poly's own frame rather than reaching through to it.
    const Mesh far = plane(-1.0f, -1, 2, -1, 2);
    bake::BakeParams p = params64();
    const bake::BakeResult missed = bake::bake(low, far, bake::BakeMap::ObjectPosition, p);
    REQUIRE_FALSE(missed.image.pixels.empty());
    // The bounds span both meshes, so the low-poly's own z = 0 is the top.
    CHECK(missed.encoding.boundsMin.z == doctest::Approx(-1.0f));
    CHECK(missed.encoding.boundsMax.z == doctest::Approx(0.0f));
    CHECK(texel(missed.image, 0.5f, 0.5f).z == doctest::Approx(1.0f));

    // Open the cage past the Target and the same bake reaches it.
    p.cageDistance = 1.5f;
    const bake::BakeResult reached = bake::bake(low, far, bake::BakeMap::ObjectPosition, p);
    CHECK(texel(reached.image, 0.5f, 0.5f).z == doctest::Approx(0.0f));
}

TEST_CASE("the ray-traced maps report progress as they accumulate") {
    const Mesh low = planeWithUv(0, 0, 1, 0, 1);
    const Mesh high = box(0, 1, 0, 1, -0.5f, 0);
    for (const bake::BakeMap map :
         {bake::BakeMap::AmbientOcclusion, bake::BakeMap::BentNormal, bake::BakeMap::Thickness}) {
        CAPTURE(static_cast<int>(map));
        std::atomic<int> reports{0};
        float last = 0.0f;
        std::mutex lock;
        ProgressSink sink([&](float value, std::string_view) {
            const std::lock_guard<std::mutex> guard(lock);
            ++reports;
            CHECK(value >= last);
            last = value;
        });
        const bake::BakeResult r = bake::bake(low, high, map, params64(), &sink);
        REQUIRE_FALSE(r.image.pixels.empty());
        CHECK(reports.load() > 5);  // not once at the end
        CHECK(last == doctest::Approx(1.0f));
    }
}

TEST_CASE("the ray-traced maps honour cancellation") {
    const Mesh low = planeWithUv(0, 0, 1, 0, 1);
    const Mesh high = box(0, 1, 0, 1, -0.5f, 0);
    for (const bake::BakeMap map : {bake::BakeMap::BentNormal, bake::BakeMap::Thickness}) {
        CAPTURE(static_cast<int>(map));
        const CancelToken token;
        token.requestCancel();
        const bake::BakeResult r = bake::bake(low, high, map, params64(), nullptr, &token);
        CHECK(r.cancelled);
    }
}

TEST_CASE("the ray-traced maps dispatch one texel loop through the compute layer") {
    // The same shape the AO bake has: the parallelism is the texel loop, and a
    // per-texel ray batch never fans out on its own. That is also what puts the
    // new maps under the compute layer's existing raycast parity harness.
    const Mesh low = planeWithUv(0, 0, 1, 0, 1);
    const Mesh high = box(0, 1, 0, 1, -0.5f, 0);
    for (const bake::BakeMap map : {bake::BakeMap::BentNormal, bake::BakeMap::Thickness}) {
        CAPTURE(static_cast<int>(map));
        auto recorder = std::make_shared<RecordingBackend>(accel::defaultBackend());
        const ScopedBackend installed(recorder);
        const bake::BakeResult r = bake::bake(low, high, map, params64());
        REQUIRE(r.texelsCovered > 1000);
        const std::vector<std::size_t> ranges = recorder->ranges();
        REQUIRE(!ranges.empty());
        CHECK(std::count(ranges.begin(), ranges.end(), r.texelsCovered) == 1);
        CHECK(*std::max_element(ranges.begin(), ranges.end()) == r.texelsCovered);
    }
}

TEST_CASE("every map reports an encoding basis") {
    const Mesh low = planeWithUv(0, 0, 1, 0, 1);
    const Mesh high = box(0, 1, 0, 1, -0.5f, 0);
    const auto basisOf = [&](bake::BakeMap map) {
        return bake::bake(low, high, map, params64()).encoding.basis;
    };
    CHECK(basisOf(bake::BakeMap::Normal) == bake::EncodingBasis::TangentNormal);
    CHECK(basisOf(bake::BakeMap::AmbientOcclusion) == bake::EncodingBasis::None);
    CHECK(basisOf(bake::BakeMap::Displacement) == bake::EncodingBasis::Distance);
    CHECK(basisOf(bake::BakeMap::Position) == bake::EncodingBasis::None);
    CHECK(basisOf(bake::BakeMap::Color) == bake::EncodingBasis::None);
    CHECK(basisOf(bake::BakeMap::Curvature) == bake::EncodingBasis::None);
    CHECK(basisOf(bake::BakeMap::Cavity) == bake::EncodingBasis::None);
    CHECK(basisOf(bake::BakeMap::ObjectNormal) == bake::EncodingBasis::ObjectNormal);
    CHECK(basisOf(bake::BakeMap::ObjectPosition) == bake::EncodingBasis::ObjectBounds);
    CHECK(basisOf(bake::BakeMap::BentNormal) == bake::EncodingBasis::TangentNormal);
    CHECK(basisOf(bake::BakeMap::Thickness) == bake::EncodingBasis::Distance);
}

TEST_CASE("uncovered texels take a neutral value per map") {
    // A quarter-covered layout, so three quarters of the image is padding. Zero
    // is not neutral for an object-space map: it decodes to a CORNER of the
    // bounds, which bleeds into the surface under dilation or mip generation.
    const Mesh low = quarterChartPlane();
    const Mesh high = plane(0, -1, 2, -1, 2);
    const bake::BakeResult position =
        bake::bake(low, high, bake::BakeMap::ObjectPosition, params64());
    const bake::BakeResult normalMap =
        bake::bake(low, high, bake::BakeMap::ObjectNormal, params64());
    const bake::BakeResult thickness = bake::bake(low, high, bake::BakeMap::Thickness, params64());
    REQUIRE_FALSE(position.image.pixels.empty());
    REQUIRE(position.texelsCovered < static_cast<std::size_t>(64 * 64) / 2);

    // The top-right corner of the image is outside the chart.
    CHECK(position.image.at(63, 0, 0) == doctest::Approx(0.5f));
    CHECK(position.image.at(63, 0, 1) == doctest::Approx(0.5f));
    CHECK(position.image.at(63, 0, 2) == doctest::Approx(0.5f));
    CHECK(normalMap.image.at(63, 0, 2) == doctest::Approx(0.5f));
    CHECK(thickness.image.at(63, 0, 0) == doctest::Approx(0.0f));
}
