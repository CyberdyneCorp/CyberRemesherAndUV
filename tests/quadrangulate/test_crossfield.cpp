#include <doctest.h>

#include <algorithm>
#include <cmath>
#include <cstdlib>
#include <vector>

#include "cyber/accel/backend.hpp"
#include "cyber/core/mesh.hpp"
#include "cyber/quadrangulate/crossfield.hpp"

using cyber::FaceId;
using cyber::Index;
using cyber::Mesh;
using cyber::Vec3;
namespace remesh = cyber::remesh;

namespace {

// Triangulated n x n grid in the z = 0 plane; boundary edges are axis-aligned.
Mesh makeGrid(int n) {
    std::vector<Vec3> p;
    for (int i = 0; i <= n; ++i) {
        for (int j = 0; j <= n; ++j) {
            p.push_back({static_cast<float>(i), static_cast<float>(j), 0.0f});
        }
    }
    auto idx = [n](int i, int j) { return static_cast<Index>(i * (n + 1) + j); };
    std::vector<std::vector<Index>> f;
    for (int i = 0; i < n; ++i) {
        for (int j = 0; j < n; ++j) {
            f.push_back({idx(i, j), idx(i + 1, j), idx(i + 1, j + 1)});
            f.push_back({idx(i, j), idx(i + 1, j + 1), idx(i, j + 1)});
        }
    }
    return Mesh::fromIndexed(p, f);
}

// Angular distance of a planar direction to the nearest grid axis, folded into
// the cross's 90-degree symmetry, in degrees.
float axisMisalignmentDeg(Vec3 d) {
    float theta = std::atan2(d.y, d.x);                        // (-pi, pi]
    float m = std::fmod(std::fabs(theta), cyber::kPi / 2.0f);  // [0, pi/2)
    m = std::fmin(m, cyber::kPi / 2.0f - m);                   // fold to nearest axis
    return m * 180.0f / cyber::kPi;
}

}  // namespace

TEST_CASE("cross field on a flat grid relaxes to the axis-aligned directions") {
    Mesh mesh = makeGrid(6);
    mesh.tagFeatureEdges(90.0f);  // tags the axis-aligned boundary as constraints
    auto backend = cyber::accel::defaultBackend();

    const remesh::CrossField field = remesh::computeCrossField(mesh, 50, *backend);
    REQUIRE(field.size() == mesh.faceCapacity());

    double totalErr = 0.0;
    std::size_t count = 0;
    for (Index i = 0; i < mesh.faceCapacity(); ++i) {
        const FaceId f{i};
        if (!mesh.isAlive(f)) {
            continue;
        }
        // The cross must stay a unit 4-symmetry vector.
        const float len = std::sqrt(field.real[i] * field.real[i] + field.imag[i] * field.imag[i]);
        REQUIRE(len == doctest::Approx(1.0f).epsilon(0.01));
        totalErr += axisMisalignmentDeg(field.direction(f));
        ++count;
    }
    REQUIRE(count > 0);
    // A boundary-constrained flat field aligns tightly to the grid axes.
    REQUIRE(totalErr / static_cast<double>(count) < 8.0);
}

TEST_CASE("orientation-derived cross field is a unit 4-RoSy aligned to a flat grid") {
    // computeCrossFieldFromOrientation (the multiresolution-field path, gated by
    // CYBER_QC_CROSSFIELD_MULTIRES in the seamless solver) must produce a valid,
    // feature-aligned per-face cross just like the single-level solver.
    Mesh mesh = makeGrid(6);
    mesh.tagFeatureEdges(90.0f);

    const remesh::CrossField field = remesh::computeCrossFieldFromOrientation(mesh, 30);
    REQUIRE(field.size() == mesh.faceCapacity());

    double totalErr = 0.0;
    std::size_t count = 0;
    for (Index i = 0; i < mesh.faceCapacity(); ++i) {
        const FaceId f{i};
        if (!mesh.isAlive(f)) {
            continue;
        }
        const float len = std::sqrt(field.real[i] * field.real[i] + field.imag[i] * field.imag[i]);
        REQUIRE(len == doctest::Approx(1.0f).epsilon(0.01));
        totalErr += axisMisalignmentDeg(field.direction(f));
        ++count;
    }
    REQUIRE(count > 0);
    REQUIRE(totalErr / static_cast<double>(count) < 8.0);
}

TEST_CASE("per-vertex Knoppel-Crane field is a unit 4-RoSy aligned to a flat grid") {
    // computeCrossFieldKnoppelCraneVertex (the paper-faithful per-vertex cotan build, gated by
    // CYBER_QC_KC_FIELD + CYBER_QC_KC_VERTEX) must produce a valid, feature-aligned per-face cross:
    // the smallest generalized eigenvector of the vertex connection Laplacian, projected to faces.
    Mesh mesh = makeGrid(6);
    mesh.tagFeatureEdges(90.0f);
    auto backend = cyber::accel::defaultBackend();

    const remesh::CrossField field = remesh::computeCrossFieldKnoppelCraneVertex(mesh, 50, *backend);
    REQUIRE(field.size() == mesh.faceCapacity());

    double totalErr = 0.0;
    std::size_t count = 0;
    for (Index i = 0; i < mesh.faceCapacity(); ++i) {
        const FaceId f{i};
        if (!mesh.isAlive(f)) {
            continue;
        }
        const float len = std::sqrt(field.real[i] * field.real[i] + field.imag[i] * field.imag[i]);
        REQUIRE(len == doctest::Approx(1.0f).epsilon(0.01));
        totalErr += axisMisalignmentDeg(field.direction(f));
        ++count;
    }
    REQUIRE(count > 0);
    REQUIRE(totalErr / static_cast<double>(count) < 8.0);
}

TEST_CASE("per-vertex Knoppel-Crane field is deterministic") {
    Mesh mesh = makeGrid(5);
    mesh.tagFeatureEdges(90.0f);
    auto backend = cyber::accel::defaultBackend();
    const remesh::CrossField a = remesh::computeCrossFieldKnoppelCraneVertex(mesh, 30, *backend);
    const remesh::CrossField b = remesh::computeCrossFieldKnoppelCraneVertex(mesh, 30, *backend);
    REQUIRE(a.real == b.real);
    REQUIRE(a.imag == b.imag);
}

TEST_CASE("per-face Knoppel-Crane field is a unit 4-RoSy aligned to a flat grid") {
    // computeCrossFieldKnoppelCrane is the SHIPPED (flag-gated) KC default: the smallest generalized
    // eigenvector of the per-face (dual) connection Laplacian, solved by inverse iteration with EXACT
    // reduced-elimination feature pins. On a flat, boundary-constrained grid the connection is flat,
    // so the globally-optimal field is the axis-aligned one — the same contract the iterative field
    // satisfies.
    Mesh mesh = makeGrid(6);
    mesh.tagFeatureEdges(90.0f);
    auto backend = cyber::accel::defaultBackend();

    const remesh::CrossField field = remesh::computeCrossFieldKnoppelCrane(mesh, 50, *backend);
    REQUIRE(field.size() == mesh.faceCapacity());

    double totalErr = 0.0;
    std::size_t count = 0;
    for (Index i = 0; i < mesh.faceCapacity(); ++i) {
        const FaceId f{i};
        if (!mesh.isAlive(f)) {
            continue;
        }
        const float len = std::sqrt(field.real[i] * field.real[i] + field.imag[i] * field.imag[i]);
        REQUIRE(len == doctest::Approx(1.0f).epsilon(0.01));
        totalErr += axisMisalignmentDeg(field.direction(f));
        ++count;
    }
    REQUIRE(count > 0);
    REQUIRE(totalErr / static_cast<double>(count) < 8.0);
}

TEST_CASE("per-face Knoppel-Crane field is deterministic") {
    Mesh mesh = makeGrid(5);
    mesh.tagFeatureEdges(90.0f);
    auto backend = cyber::accel::defaultBackend();
    const remesh::CrossField a = remesh::computeCrossFieldKnoppelCrane(mesh, 30, *backend);
    const remesh::CrossField b = remesh::computeCrossFieldKnoppelCrane(mesh, 30, *backend);
    REQUIRE(a.real == b.real);
    REQUIRE(a.imag == b.imag);
}

TEST_CASE("cross field is deterministic") {
    Mesh mesh = makeGrid(5);
    mesh.tagFeatureEdges(90.0f);
    auto backend = cyber::accel::defaultBackend();
    const remesh::CrossField a = remesh::computeCrossField(mesh, 30, *backend);
    const remesh::CrossField b = remesh::computeCrossField(mesh, 30, *backend);
    REQUIRE(a.real == b.real);
    REQUIRE(a.imag == b.imag);
}

TEST_CASE("cross field on an empty mesh is empty") {
    Mesh mesh;
    auto backend = cyber::accel::defaultBackend();
    const remesh::CrossField field = remesh::computeCrossField(mesh, 10, *backend);
    REQUIRE(field.size() == 0);
}

// Crease alignment of the field, and its planarity gate (docs/ROADMAP.md Phase 3, lever c2).
//
// `creaseAlignDegrees` widens the set of edges the field aligns to WITHOUT widening the hard-seam
// set. It must apply on a CURVED surface and be suppressed on a FLAT one: a planar panel's cross
// field is degenerate (every orientation is equally smooth), so pinning its border imposes
// arbitrary structure the interior cannot reconcile — measured ungated, that took a subdivided
// cube's edge CV from 0.201 to 0.397.
namespace {

// A sheet folded 90 degrees along a straight crease at s = 0. Each panel is a cylinder section of
// radius `radius` (<= 0 gives a flat panel), so the curvature is CONSTANT — including right at the
// crease, which is where the planarity gate looks. A quadratic bend would be flat exactly there
// and would read as planar however large its coefficient.
Mesh makeFoldedSheet(float radius, float shear = 0.0f) {
    std::vector<Vec3> p;
    std::vector<std::vector<cyber::Index>> f;
    const int n = 12;
    for (int i = 0; i <= n; ++i) {
        const float t = static_cast<float>(i) / static_cast<float>(n);
        for (int j = 0; j <= n; ++j) {
            const float s = static_cast<float>(j) / static_cast<float>(n) * 2.0f - 1.0f;
            const float u = std::abs(s);  // arc length away from the crease
            float a = u, b = 0.0f;        // flat: runs straight out, no rise
            if (radius > 0.0f) {
                a = radius * std::sin(u / radius);
                b = radius * (1.0f - std::cos(u / radius));
            }
            // s <= 0 runs out in +y rising in +z; s > 0 runs up in +z rising in +y.
            // `shear` slews the parametrisation across the crease so the relaxed field is NOT
            // already crease-aligned; without it this fixture cannot discriminate, because the
            // smooth solution lands on the crease direction anyway and pinning changes nothing.
            const float ts = t + shear * s;
            p.push_back(s <= 0.0f ? Vec3{ts, a, b} : Vec3{ts, b, a});
        }
    }
    const auto id = [&](int i, int j) { return static_cast<cyber::Index>(i * (n + 1) + j); };
    for (int i = 0; i < n; ++i) {
        for (int j = 0; j < n; ++j) {
            f.push_back({id(i, j), id(i + 1, j), id(i + 1, j + 1)});
            f.push_back({id(i, j), id(i + 1, j + 1), id(i, j + 1)});
        }
    }
    return Mesh::fromIndexed(p, f);
}

// Median angle between the field's cross and the crease direction (+x), over faces touching the
// crease. A 4-RoSy cross is invariant under quarter turns, so the deviation folds into [0, 45].
float creaseDeviationDeg(const Mesh& mesh, const remesh::CrossField& field) {
    std::vector<float> devs;
    for (cyber::Index i = 0; i < mesh.edgeCapacity(); ++i) {
        const cyber::EdgeId e{i};
        if (!mesh.isAlive(e) || mesh.edgeFaceCount(e) != 2) {
            continue;
        }
        const std::vector<cyber::FaceId> ef = mesh.edgeFaces(e);
        const Vec3 n0 = normalized(mesh.faceNormal(ef[0]));
        const Vec3 n1 = normalized(mesh.faceNormal(ef[1]));
        if (dot(n0, n1) >= std::cos(45.0f * cyber::kPi / 180.0f)) {
            continue;  // not a crease
        }
        for (const cyber::FaceId face : ef) {
            const Vec3 d = normalized(field.direction(face));
            float a = std::acos(std::clamp(std::abs(d.x), 0.0f, 1.0f)) * 180.0f / cyber::kPi;
            a = std::fmod(a, 90.0f);
            devs.push_back(std::min(a, 90.0f - a));
        }
    }
    REQUIRE(!devs.empty());
    std::sort(devs.begin(), devs.end());
    return devs[devs.size() / 2];
}

}  // namespace

// Count faces whose cross actually moved between two solves.
std::size_t differingFaces(const Mesh& mesh, const remesh::CrossField& a,
                           const remesh::CrossField& b) {
    std::size_t diff = 0;
    for (std::size_t i = 0; i < a.real.size(); ++i) {
        if (!mesh.isAlive(cyber::FaceId{static_cast<cyber::Index>(i)})) {
            continue;
        }
        if (std::abs(a.real[i] - b.real[i]) > 1e-5f || std::abs(a.imag[i] - b.imag[i]) > 1e-5f) {
            ++diff;
        }
    }
    return diff;
}

TEST_CASE("crease alignment applies on a curved surface and is gated off on a planar one") {
    auto backend = cyber::accel::defaultBackend();

    // Curved: cylinder-section panels, so there is real curvature AT the crease (a quadratic bend
    // would be flat exactly there and read as planar however large its coefficient).
    const Mesh curved = makeFoldedSheet(0.8f, 0.6f);
    const remesh::CrossField cOff = remesh::computeCrossField(curved, 120, *backend, 0.0f);
    const remesh::CrossField cOn = remesh::computeCrossField(curved, 120, *backend, 45.0f);
    const std::size_t curvedDiff = differingFaces(curved, cOff, cOn);
    CAPTURE(curvedDiff);
    CHECK(curvedDiff > 0);                          // the gate admits the pins here
    CHECK(creaseDeviationDeg(curved, cOn) < 5.0f);  // and the result stays crease-aligned

    // Planar: a flat panel's field is degenerate, so a crease pin would impose arbitrary
    // structure. Ungated, exactly this case regressed flat CAD (cube edge CV 0.201 -> 0.397).
    // The gate must suppress it and leave the solve untouched.
    const Mesh flatSheet = makeFoldedSheet(0.0f, 0.6f);
    const remesh::CrossField fOff = remesh::computeCrossField(flatSheet, 120, *backend, 0.0f);
    const remesh::CrossField fOn = remesh::computeCrossField(flatSheet, 120, *backend, 45.0f);
    CHECK(differingFaces(flatSheet, fOff, fOn) == 0);
}

TEST_CASE("per-face Knoppel-Crane holds crease pins exactly and differs from the iterative field") {
    // The KC per-face field imposes the feature/crease pins by EXACT reduced elimination (constrained
    // faces removed from the eigen-solve, held at their exact pin), so the crease faces must align to
    // the crease at least as tightly as the iterative field — the c1/c2 hard-constraint contract.
    auto backend = cyber::accel::defaultBackend();
    const Mesh curved = makeFoldedSheet(0.8f, 0.6f);

    const remesh::CrossField iter = remesh::computeCrossField(curved, 120, *backend, 45.0f);
    const remesh::CrossField kc = remesh::computeCrossFieldKnoppelCrane(curved, 120, *backend, 45.0f);

    // Exact pins keep the crease faces crease-aligned (the hard c1/c2 contract).
    CHECK(creaseDeviationDeg(curved, kc) < 5.0f);
    // And the globally-optimal interior is a genuinely different field than local relaxation
    // (guards against a silent fall-back-to-seed that would make KC a no-op).
    CHECK(differingFaces(curved, iter, kc) > 0);
}

TEST_CASE("KC feature-alignment band keeps crease pins, stays unit, and shifts the interior") {
    // The opt-in feature-band (CYBER_QC_KC_FEATURE_BAND, lever c7 (iv)) softly pulls the free faces
    // within a few dual-rings of a crease pin back toward the feature-following iterative seed. It
    // must (a) still hold the hard crease pins, (b) stay a unit 4-RoSy field, and (c) actually change
    // the near-crease interior vs the no-band KC field (proving the band is active, not a no-op).
    auto backend = cyber::accel::defaultBackend();
    const Mesh curved = makeFoldedSheet(0.8f, 0.6f);

    const remesh::CrossField kcNoBand = remesh::computeCrossFieldKnoppelCrane(curved, 120, *backend, 45.0f);

    setenv("CYBER_QC_KC_FEATURE_BAND", "1", 1);
    const remesh::CrossField kcBand = remesh::computeCrossFieldKnoppelCrane(curved, 120, *backend, 45.0f);
    unsetenv("CYBER_QC_KC_FEATURE_BAND");

    // (a) hard crease pins still hold, and (b) the field is still unit-normalised.
    CHECK(creaseDeviationDeg(curved, kcBand) < 5.0f);
    for (Index i = 0; i < curved.faceCapacity(); ++i) {
        const FaceId f{i};
        if (!curved.isAlive(f)) {
            continue;
        }
        const float len =
            std::sqrt(kcBand.real[i] * kcBand.real[i] + kcBand.imag[i] * kcBand.imag[i]);
        CHECK(len == doctest::Approx(1.0f).epsilon(0.02));
    }
    // (c) the band moved the interior relative to the no-band KC field.
    CHECK(differingFaces(curved, kcNoBand, kcBand) > 0);
}
