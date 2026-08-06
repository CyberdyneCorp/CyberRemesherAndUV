#include <doctest.h>

#include <cmath>
#include <cstring>
#include <vector>

#include "cyber/accel/backend.hpp"
#include "cyber/core/guidance.hpp"
#include "cyber/core/mesh.hpp"
#include "cyber/quadrangulate/crossfield.hpp"

using cyber::FaceId;
using cyber::Index;
using cyber::Mesh;
using cyber::Vec3;
using cyber::VertexId;
namespace remesh = cyber::remesh;

namespace {

// Triangulated n x n grid on [0,n]^2 in the z = 0 plane.
Mesh makeFlatGrid(int n) {
    std::vector<Vec3> p;
    for (int i = 0; i <= n; ++i) {
        for (int j = 0; j <= n; ++j) {
            p.push_back({static_cast<float>(i), static_cast<float>(j), 0.0f});
        }
    }
    const auto idx = [n](int i, int j) { return static_cast<Index>(i * (n + 1) + j); };
    std::vector<std::vector<Index>> f;
    for (int i = 0; i < n; ++i) {
        for (int j = 0; j < n; ++j) {
            f.push_back({idx(i, j), idx(i + 1, j), idx(i + 1, j + 1)});
            f.push_back({idx(i, j), idx(i + 1, j + 1), idx(i, j + 1)});
        }
    }
    return Mesh::fromIndexed(p, f);
}

Vec3 faceCentroid(const Mesh& mesh, FaceId f) {
    Vec3 c{0, 0, 0};
    for (const VertexId v : mesh.faceVertices(f)) {
        c += mesh.position(v);
    }
    return c / static_cast<float>(mesh.faceVertices(f).size());
}

// Angle between two planar directions folded into the cross field's 90-degree
// symmetry: 0 means the cross is aligned with `ref`, 45 is the worst case.
float crossDeviationDeg(Vec3 d, Vec3 ref) {
    float a = std::atan2(d.y, d.x) - std::atan2(ref.y, ref.x);
    a = std::fabs(std::fmod(a, cyber::kPi / 2.0f));
    if (a > cyber::kPi / 4.0f) {
        a = cyber::kPi / 2.0f - a;
    }
    return cyber::radiansToDegrees(a);
}

// A guide running diagonally through the middle of the grid.
remesh::FlowGuide diagonalGuide(float n, float radius, float strength) {
    remesh::FlowGuide g;
    g.points = {Vec3{0.15f * n, 0.15f * n, 0.0f}, Vec3{0.85f * n, 0.85f * n, 0.0f}};
    g.strength = strength;
    g.radius = radius;
    return g;
}

bool fieldBitsEqual(const remesh::CrossField& a, const remesh::CrossField& b) {
    if (a.real.size() != b.real.size() || a.imag.size() != b.imag.size()) {
        return false;
    }
    return std::memcmp(a.real.data(), b.real.data(), a.real.size() * sizeof(float)) == 0 &&
           std::memcmp(a.imag.data(), b.imag.data(), a.imag.size() * sizeof(float)) == 0;
}

}  // namespace

TEST_CASE("null guidance leaves the cross field bit-identical") {
    const Mesh grid = makeFlatGrid(8);
    auto backend = cyber::accel::defaultBackend();

    const remesh::CrossField base = remesh::computeCrossField(grid, 60, *backend);

    SUBCASE("explicit nullptr") {
        const remesh::CrossField withNull =
            remesh::computeCrossField(grid, 60, *backend, 45.0f, nullptr, nullptr);
        CHECK(fieldBitsEqual(base, withNull));
    }
    SUBCASE("empty guidance object") {
        const remesh::GuidanceField empty(grid, remesh::Guidance{});
        CHECK(empty.empty());
        const remesh::CrossField withEmpty =
            remesh::computeCrossField(grid, 60, *backend, 45.0f, nullptr, &empty);
        CHECK(fieldBitsEqual(base, withEmpty));
        CHECK(withEmpty.guidedFaces == 0);
    }
    SUBCASE("guide with strength 0 biases nothing") {
        remesh::Guidance g;
        g.guides.push_back(diagonalGuide(8.0f, 3.0f, 0.0f));
        const remesh::GuidanceField field(grid, g);
        CHECK(field.hasGuides());
        const remesh::CrossField zeroStrength =
            remesh::computeCrossField(grid, 60, *backend, 45.0f, nullptr, &field);
        CHECK(fieldBitsEqual(base, zeroStrength));
        CHECK(zeroStrength.guidedFaces == 0);
    }
    SUBCASE("multires path is bit-identical too") {
        const remesh::CrossField mbase =
            remesh::computeCrossFieldFromOrientation(grid, 60, *backend);
        const remesh::GuidanceField empty(grid, remesh::Guidance{});
        const remesh::CrossField mempty =
            remesh::computeCrossFieldFromOrientation(grid, 60, *backend, 45.0f, nullptr, &empty);
        CHECK(fieldBitsEqual(mbase, mempty));
    }
}

TEST_CASE("guide bias rotates the interior cross field toward the guide tangent") {
    constexpr int kN = 12;
    const Mesh grid = makeFlatGrid(kN);
    auto backend = cyber::accel::defaultBackend();

    // A 45-degree guide down the diagonal, deliberately fighting the grid's
    // axis-aligned boundary pins.
    remesh::Guidance g;
    g.guides.push_back(diagonalGuide(static_cast<float>(kN), 2.0f, 1.0f));
    const remesh::GuidanceField field(grid, g);

    const remesh::CrossField unguided = remesh::computeCrossField(grid, 200, *backend);
    const remesh::CrossField guided =
        remesh::computeCrossField(grid, 200, *backend, 45.0f, nullptr, &field);

    CHECK(guided.guidedFaces > 0);

    const Vec3 tangent = cyber::normalized(Vec3{1.0f, 1.0f, 0.0f});
    double sumGuided = 0.0;
    double sumUnguided = 0.0;
    std::size_t inRadius = 0;
    for (Index fi = 0; fi < grid.faceCapacity(); ++fi) {
        const FaceId f{fi};
        if (!grid.isAlive(f)) {
            continue;
        }
        const Vec3 c = faceCentroid(grid, f);
        if (field.guideAt(c, cyber::normalized(grid.faceNormal(f))).weight <= 0.0f) {
            continue;
        }
        ++inRadius;
        sumGuided += static_cast<double>(crossDeviationDeg(guided.direction(f), tangent));
        sumUnguided += static_cast<double>(crossDeviationDeg(unguided.direction(f), tangent));
    }
    REQUIRE(inRadius > 0);
    const double meanGuided = sumGuided / static_cast<double>(inRadius);
    const double meanUnguided = sumUnguided / static_cast<double>(inRadius);
    MESSAGE("in-radius mean deviation from guide tangent: unguided "
            << meanUnguided << " deg, guided " << meanGuided << " deg");
    // The axis-aligned unguided field sits ~45 degrees off a 45-degree guide;
    // the guided field must be substantially closer to it.
    CHECK(meanGuided < meanUnguided);
    CHECK(meanGuided < 15.0);
}

TEST_CASE("guide influence stops at the radius") {
    constexpr int kN = 14;
    const Mesh grid = makeFlatGrid(kN);
    auto backend = cyber::accel::defaultBackend();

    remesh::Guidance g;
    remesh::FlowGuide guide;
    // A short stroke in one corner: everything past the radius must be
    // unaffected by the bias itself (the smoothing still diffuses, so the test
    // asserts on the BIAS reach, not on the solved field far away).
    guide.points = {Vec3{1.0f, 1.0f, 0.0f}, Vec3{3.0f, 1.0f, 0.0f}};
    guide.strength = 1.0f;
    guide.radius = 1.5f;
    g.guides.push_back(guide);
    const remesh::GuidanceField field(grid, g);

    std::size_t inside = 0;
    for (Index fi = 0; fi < grid.faceCapacity(); ++fi) {
        const FaceId f{fi};
        if (!grid.isAlive(f)) {
            continue;
        }
        const Vec3 c = faceCentroid(grid, f);
        const float w = field.guideAt(c, cyber::normalized(grid.faceNormal(f))).weight;
        // Distance from the centroid to the stroke segment.
        const float t = std::clamp((c.x - 1.0f) / 2.0f, 0.0f, 1.0f);
        const Vec3 q{1.0f + 2.0f * t, 1.0f, 0.0f};
        const float d = cyber::length(c - q);
        if (d >= guide.radius) {
            CHECK(w == 0.0f);
        } else {
            CHECK(w > 0.0f);
            ++inside;
        }
    }
    CHECK(inside > 0);
}

TEST_CASE("a guide crossing a hard pin does not override it and the conflict is counted") {
    constexpr int kN = 10;
    Mesh grid = makeFlatGrid(kN);
    // Tag a straight interior run of edges as features: those faces are hard
    // pinned, so a guide crossing them must lose and be reported.
    std::size_t tagged = 0;
    for (Index ei = 0; ei < grid.edgeCapacity(); ++ei) {
        const cyber::EdgeId e{ei};
        if (!grid.isAlive(e) || grid.edgeFaceCount(e) != 2) {
            continue;
        }
        const auto [a, b] = grid.edgeVertices(e);
        const Vec3 pa = grid.position(a);
        const Vec3 pb = grid.position(b);
        if (pa.y == 5.0f && pb.y == 5.0f) {
            grid.setFeatureEdge(e, true);
            ++tagged;
        }
    }
    REQUIRE(tagged > 0);

    auto backend = cyber::accel::defaultBackend();
    remesh::Guidance g;
    remesh::FlowGuide guide;
    guide.points = {Vec3{5.0f, 1.0f, 0.0f}, Vec3{5.0f, 9.0f, 0.0f}};  // crosses the crease
    guide.strength = 1.0f;
    guide.radius = 2.0f;
    g.guides.push_back(guide);
    const remesh::GuidanceField field(grid, g);

    const remesh::CrossField guided =
        remesh::computeCrossField(grid, 120, *backend, 45.0f, nullptr, &field);
    CHECK(guided.guideConflictFaces > 0);
    // guidedFaces is 0 here on purpose: a flat panel's crease pins flood-fill
    // across the whole coplanar patch (applyPins), so every face the guide
    // reaches is hard-pinned. That is exactly the case the conflict count
    // exists to report — the guide is neither applied nor silently dropped.
    CHECK(guided.guidedFaces == 0);

    // Every hard-pinned face the guide reached still carries its pin's angle,
    // i.e. it is aligned with the tagged (x-direction) crease.
    const remesh::CrossField unguided = remesh::computeCrossField(grid, 120, *backend);
    for (Index fi = 0; fi < grid.faceCapacity(); ++fi) {
        const FaceId f{fi};
        if (!grid.isAlive(f)) {
            continue;
        }
        bool onFeature = false;
        const std::vector<VertexId> fv = grid.faceVertices(f);
        for (std::size_t k = 0; k < fv.size(); ++k) {
            const cyber::EdgeId e = grid.edgeBetween(fv[k], fv[(k + 1) % fv.size()]);
            if (e.valid() && grid.isFeatureEdge(e)) {
                onFeature = true;
            }
        }
        if (!onFeature) {
            continue;
        }
        CHECK(guided.real[fi] == doctest::Approx(unguided.real[fi]).epsilon(1e-5));
        CHECK(guided.imag[fi] == doctest::Approx(unguided.imag[fi]).epsilon(1e-5));
    }
}

TEST_CASE("guidance field density sampling honors the documented clamp") {
    const Mesh grid = makeFlatGrid(4);
    remesh::Guidance g;
    // Per-vertex density: 4.0 on the left half, 1.0 on the right, plus one
    // out-of-range value the FIELD clamps (validateGuidance clamps upstream,
    // but the sampler must never emit an unclamped multiplier either).
    for (Index vi = 0; vi < grid.vertexCapacity(); ++vi) {
        const Vec3 p = grid.position(VertexId{vi});
        g.density.vertexValues.push_back(p.x < 2.0f ? 100.0f : 1.0f);
    }
    const remesh::GuidanceField field(grid, g);
    CHECK(field.hasDensity());
    CHECK(field.densityAt(Vec3{0.5f, 2.0f, 0.0f}) == doctest::Approx(remesh::kDensityMax));
    CHECK(field.densityAt(Vec3{3.5f, 2.0f, 0.0f}) == doctest::Approx(1.0f));
    // No density supplied -> a neutral 1.0 multiplier everywhere.
    const remesh::GuidanceField none(grid, remesh::Guidance{});
    CHECK(none.densityAt(Vec3{1.0f, 1.0f, 0.0f}) == 1.0f);
}
