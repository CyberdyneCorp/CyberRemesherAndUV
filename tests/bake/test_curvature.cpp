#include <doctest.h>

#include <algorithm>
#include <cmath>
#include <functional>
#include <vector>

#include "cyber/bake/bake.hpp"
#include "cyber/bake/curvature.hpp"
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

// Closed UV sphere of radius `r`, outward-wound. Poles are single vertices;
// the bands between them are quads (the estimator fan-triangulates them).
Mesh makeSphere(float r, int rings, int segments) {
    std::vector<Vec3> pos;
    const auto ringVertex = [&](int i, int j) -> Index {
        return static_cast<Index>(1 + (i - 1) * segments + (j % segments));
    };
    pos.push_back({0, 0, r});  // north pole = index 0
    for (int i = 1; i < rings; ++i) {
        const float theta = cyber::kPi * static_cast<float>(i) / static_cast<float>(rings);
        for (int j = 0; j < segments; ++j) {
            const float phi =
                2.0f * cyber::kPi * static_cast<float>(j) / static_cast<float>(segments);
            pos.push_back({r * std::sin(theta) * std::cos(phi), r * std::sin(theta) * std::sin(phi),
                           r * std::cos(theta)});
        }
    }
    const Index south = static_cast<Index>(pos.size());
    pos.push_back({0, 0, -r});

    std::vector<std::vector<Index>> faces;
    for (int j = 0; j < segments; ++j) {
        faces.push_back({0, ringVertex(1, j), ringVertex(1, j + 1)});
    }
    for (int i = 1; i + 1 < rings; ++i) {
        for (int j = 0; j < segments; ++j) {
            faces.push_back({ringVertex(i, j), ringVertex(i + 1, j), ringVertex(i + 1, j + 1),
                             ringVertex(i, j + 1)});
        }
    }
    for (int j = 0; j < segments; ++j) {
        faces.push_back({south, ringVertex(rings - 1, j + 1), ringVertex(rings - 1, j)});
    }
    return Mesh::fromIndexed(pos, faces);
}

// Height-field grid z = f(x, y) over [x0,x1]^2, quad-wound with +Z normals.
// `n` is the vertex count per side.
Mesh makeHeightField(int n, float x0, float x1, const std::function<float(float, float)>& f,
                     bool withUv) {
    std::vector<Vec3> pos;
    pos.reserve(static_cast<std::size_t>(n) * static_cast<std::size_t>(n));
    for (int iy = 0; iy < n; ++iy) {
        for (int ix = 0; ix < n; ++ix) {
            const float u = static_cast<float>(ix) / static_cast<float>(n - 1);
            const float v = static_cast<float>(iy) / static_cast<float>(n - 1);
            const float x = x0 + (x1 - x0) * u;
            const float y = x0 + (x1 - x0) * v;
            pos.push_back({x, y, f(x, y)});
        }
    }
    std::vector<std::vector<Index>> faces;
    const auto at = [n](int ix, int iy) { return static_cast<Index>(iy * n + ix); };
    for (int iy = 0; iy + 1 < n; ++iy) {
        for (int ix = 0; ix + 1 < n; ++ix) {
            faces.push_back({at(ix, iy), at(ix + 1, iy), at(ix + 1, iy + 1), at(ix, iy + 1)});
        }
    }
    Mesh mesh = Mesh::fromIndexed(pos, faces);
    if (withUv) {
        auto& uv = mesh.cornerAttributes().create<Vec2>("uv");
        for (Index fi = 0; fi < mesh.faceCapacity(); ++fi) {
            if (!mesh.isAlive(FaceId{fi})) {
                continue;
            }
            for (const LoopId l : mesh.faceLoops(FaceId{fi})) {
                const Vec3 p = mesh.position(mesh.loopVertex(l));
                uv[l.value] = {(p.x - x0) / (x1 - x0), (p.y - x0) / (x1 - x0)};
            }
        }
    }
    return mesh;
}

// Mean |curvature| over vertices whose |z| is below `zMax` — the equatorial
// band of a UV sphere, away from the pole fans.
float equatorialCurvature(const Mesh& mesh, const std::vector<float>& h, float zMax) {
    double sum = 0.0;
    int count = 0;
    for (Index vi = 0; vi < mesh.vertexCapacity(); ++vi) {
        if (!mesh.isAlive(VertexId{vi}) || std::fabs(mesh.position(VertexId{vi}).z) > zMax) {
            continue;
        }
        sum += h[vi];
        ++count;
    }
    return count == 0 ? 0.0f : static_cast<float>(sum / count);
}

bake::BakeParams params64() {
    bake::BakeParams p;
    p.width = 64;
    p.height = 64;
    p.cageDistance = 0.1f;
    return p;
}

// Value at (u, v) of a single-channel map.
float sampleUv(const bake::Image& img, float u, float v) {
    const int px = static_cast<int>(u * static_cast<float>(img.width));
    const int py = static_cast<int>((1.0f - v) * static_cast<float>(img.height));
    return img.at(std::min(px, img.width - 1), std::min(py, img.height - 1), 0);
}

// A corrugated Target over the unit square: one full sine period along x, so
// there is a convex crest at u = 0.25 and a concave trough at u = 0.75. The
// amplitude stays inside the 0.1 cage of params64().
Mesh makeCorrugation(float amplitude, float zOffset) {
    return makeHeightField(
        65, 0.0f, 1.0f,
        [amplitude, zOffset](float x, float) {
            return zOffset + amplitude * std::sin(2.0f * cyber::kPi * x);
        },
        /*withUv=*/false);
}

}  // namespace

// ---- estimator (analytic shapes) ---------------------------------------

TEST_CASE("mean curvature of a sphere is 1/r and positive (convex)") {
    const float r = 2.0f;
    const Mesh sphere = makeSphere(r, 40, 40);
    const std::vector<float> h = bake::vertexMeanCurvature(sphere);
    // Away from the pole fans the discrete operator should land on 1/r.
    const float mean = equatorialCurvature(sphere, h, 0.5f * r);
    REQUIRE(mean == doctest::Approx(1.0f / r).epsilon(0.05));
    REQUIRE(mean > 0.0f);  // convex reads positive
}

TEST_CASE("mean curvature of a plane is zero") {
    const Mesh plane = makeHeightField(17, -0.5f, 0.5f, [](float, float) { return 0.0f; }, false);
    const std::vector<float> h = bake::vertexMeanCurvature(plane);
    for (Index vi = 0; vi < plane.vertexCapacity(); ++vi) {
        if (plane.isAlive(VertexId{vi})) {
            REQUIRE(std::fabs(h[vi]) < 1e-5f);
        }
    }
    REQUIRE(bake::curvatureScale(h) == 0.0f);  // nothing to normalize against
}

TEST_CASE("mean curvature of a symmetric saddle vanishes at the seat") {
    // z = x^2 - y^2: principal curvatures cancel at the origin, so H(0,0) = 0
    // even though the surface is far from flat there.
    const Mesh saddle =
        makeHeightField(33, -0.5f, 0.5f, [](float x, float y) { return x * x - y * y; }, false);
    const std::vector<float> h = bake::vertexMeanCurvature(saddle);
    const Index center = static_cast<Index>(33 * 16 + 16);
    REQUIRE(saddle.position(VertexId{center}).x == doctest::Approx(0.0f).epsilon(1e-4));
    REQUIRE(saddle.position(VertexId{center}).y == doctest::Approx(0.0f).epsilon(1e-4));
    REQUIRE(std::fabs(h[center]) < 0.05f);
    // The seat is flat in the mean, but the surface is not: the field is alive
    // away from it. |H| peaks analytically at 0.354, on the x axis at x = +-0.5
    // (H = (f_xx + (1+f_x^2) f_yy) / (2 (1+f_x^2)^{3/2}) = -2 / 2*2^{3/2}).
    REQUIRE(bake::curvatureScale(h) == doctest::Approx(0.32f).epsilon(0.15));
}

TEST_CASE("curvature percentile ignores one pinched outlier") {
    std::vector<float> field(100, 1.0f);
    field[7] = 1000.0f;
    REQUIRE(bake::curvatureScale(field, 0.95f) == doctest::Approx(1.0f));
    REQUIRE(bake::curvatureScale({}) == 0.0f);
}

// ---- bake encodings -----------------------------------------------------

TEST_CASE("curvature bake distinguishes convex crests from concave troughs") {
    const Mesh low =
        makeHeightField(9, 0.0f, 1.0f, [](float, float) { return 0.0f; }, /*withUv=*/true);
    const Mesh high = makeCorrugation(0.02f, 0.0f);
    const bake::BakeResult r = bake::bake(low, high, bake::BakeMap::Curvature, params64());
    REQUIRE_FALSE(r.image.pixels.empty());
    REQUIRE(r.image.channels == 1);

    const float crest = sampleUv(r.image, 0.25f, 0.5f);
    const float trough = sampleUv(r.image, 0.75f, 0.5f);
    REQUIRE(crest > 0.7f);   // convex reads brighter than mid-gray
    REQUIRE(trough < 0.3f);  // concave reads darker
    REQUIRE(crest > trough);
    // The inflection between them sits near mid-gray.
    REQUIRE(sampleUv(r.image, 0.5f, 0.5f) == doctest::Approx(0.5f).epsilon(0.3));
}

TEST_CASE("cavity bake encodes concavity only") {
    const Mesh low =
        makeHeightField(9, 0.0f, 1.0f, [](float, float) { return 0.0f; }, /*withUv=*/true);
    const Mesh high = makeCorrugation(0.02f, 0.0f);
    const bake::BakeResult r = bake::bake(low, high, bake::BakeMap::Cavity, params64());
    REQUIRE(r.image.channels == 1);

    REQUIRE(sampleUv(r.image, 0.25f, 0.5f) > 0.95f);  // convex crest reads white
    REQUIRE(sampleUv(r.image, 0.75f, 0.5f) < 0.3f);   // concave trough reads dark
    REQUIRE(sampleUv(r.image, 0.5f, 0.5f) > 0.8f);    // near-flat reads white
}

TEST_CASE("a flat target bakes to the neutral value for both variants") {
    const Mesh low =
        makeHeightField(9, 0.0f, 1.0f, [](float, float) { return 0.0f; }, /*withUv=*/true);
    const Mesh high = makeHeightField(17, 0.0f, 1.0f, [](float, float) { return 0.0f; }, false);

    const bake::BakeResult curv = bake::bake(low, high, bake::BakeMap::Curvature, params64());
    REQUIRE(sampleUv(curv.image, 0.5f, 0.5f) == doctest::Approx(0.5f).epsilon(0.02));
    const bake::BakeResult cav = bake::bake(low, high, bake::BakeMap::Cavity, params64());
    REQUIRE(sampleUv(cav.image, 0.5f, 0.5f) == doctest::Approx(1.0f).epsilon(0.02));
}

TEST_CASE("an explicit curvature range overrides the auto percentile") {
    const Mesh low =
        makeHeightField(9, 0.0f, 1.0f, [](float, float) { return 0.0f; }, /*withUv=*/true);
    const Mesh high = makeCorrugation(0.02f, 0.0f);

    bake::BakeParams wide = params64();
    wide.curvatureRange = 1e6f;  // nothing comes close -> everything is mid-gray
    const bake::BakeResult r = bake::bake(low, high, bake::BakeMap::Curvature, wide);
    REQUIRE(sampleUv(r.image, 0.25f, 0.5f) == doctest::Approx(0.5f).epsilon(0.02));
    REQUIRE(sampleUv(r.image, 0.75f, 0.5f) == doctest::Approx(0.5f).epsilon(0.02));
}

// ---- parity with the other maps -----------------------------------------

TEST_CASE("curvature follows the cage exactly as the normal bake does") {
    const Mesh low =
        makeHeightField(9, 0.0f, 1.0f, [](float, float) { return 0.0f; }, /*withUv=*/true);
    // Target lifted 0.3 above the low-poly: out of reach of a 0.1 cage.
    const Mesh high = makeCorrugation(0.02f, 0.3f);

    bake::BakeParams tight = params64();
    const bake::BakeResult nTight = bake::bake(low, high, bake::BakeMap::Normal, tight);
    const bake::BakeResult cTight = bake::bake(low, high, bake::BakeMap::Curvature, tight);
    // Both fall back to their flat defaults: no cage hit, no signal.
    REQUIRE(sampleUv(nTight.image, 0.25f, 0.5f) == doctest::Approx(0.5f).epsilon(0.02));  // R
    REQUIRE(sampleUv(cTight.image, 0.25f, 0.5f) == doctest::Approx(0.5f).epsilon(0.02));
    REQUIRE(sampleUv(cTight.image, 0.75f, 0.5f) == doctest::Approx(0.5f).epsilon(0.02));

    // Widening the cage to reach the Target picks the crest/trough back up.
    bake::BakeParams wide = params64();
    wide.cageDistance = 0.4f;
    const bake::BakeResult cWide = bake::bake(low, high, bake::BakeMap::Curvature, wide);
    REQUIRE(sampleUv(cWide.image, 0.25f, 0.5f) > 0.7f);
    REQUIRE(sampleUv(cWide.image, 0.75f, 0.5f) < 0.3f);
}

TEST_CASE("curvature and cavity bakes honor cooperative cancellation") {
    const Mesh low =
        makeHeightField(9, 0.0f, 1.0f, [](float, float) { return 0.0f; }, /*withUv=*/true);
    const Mesh high = makeCorrugation(0.02f, 0.0f);
    CancelToken cancel;
    cancel.requestCancel();
    REQUIRE(
        bake::bake(low, high, bake::BakeMap::Curvature, params64(), nullptr, &cancel).cancelled);
    REQUIRE(bake::bake(low, high, bake::BakeMap::Cavity, params64(), nullptr, &cancel).cancelled);
}
