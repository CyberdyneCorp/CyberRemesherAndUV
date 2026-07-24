#include <doctest.h>

#include <algorithm>
#include <cmath>
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

// Crease alignment of the field (docs/ROADMAP.md Phase 3, lever c2).
//
// The field is constrained to run along feature edges, but `isFeatureEdge` also decides the hard
// seams, so widening it to catch ordinary creases shreds the surface into seam-bounded patches
// (measured: median 83.4 -> 77.5). CYBER_QC_FIELD_CREASE_DEG instead widens ONLY the alignment
// set, leaving the seam set alone.
//
// This pins the property that motivated the knob: the assumption that our field was already
// crease-aligned was measured FALSE on real CAD input (fandisk's median 4-RoSy deviation from its
// creases was ~21 degrees, against ~22.5 for a random field). Here a creased fixture must come out
// measurably better aligned with the knob on than with it off.
TEST_CASE("cross field aligns to creases when the alignment set is widened") {
    // Two planes meeting at 90 degrees: a single straight crease down the middle.
    std::vector<Vec3> p;
    std::vector<std::vector<cyber::Index>> f;
    const int n = 12;
    for (int i = 0; i <= n; ++i) {
        const float t = static_cast<float>(i) / static_cast<float>(n);
        for (int j = 0; j <= n; ++j) {
            const float s = static_cast<float>(j) / static_cast<float>(n) * 2.0f - 1.0f;
            // s < 0 lies in the z=0 plane, s > 0 folds up into the y=0 plane.
            p.push_back(s <= 0.0f ? Vec3{t, -s, 0.0f} : Vec3{t, 0.0f, s});
        }
    }
    const auto id = [&](int i, int j) { return static_cast<cyber::Index>(i * (n + 1) + j); };
    for (int i = 0; i < n; ++i) {
        for (int j = 0; j < n; ++j) {
            f.push_back({id(i, j), id(i + 1, j), id(i + 1, j + 1)});
            f.push_back({id(i, j), id(i + 1, j + 1), id(i, j + 1)});
        }
    }
    const Mesh mesh = Mesh::fromIndexed(p, f);
    auto backend = cyber::accel::defaultBackend();

    // Median angle between the field's cross and the crease direction, over faces touching the
    // crease. The crease runs along +x, and a 4-RoSy cross is invariant under 90-degree turns, so
    // the deviation folds into [0, 45].
    const auto creaseDeviationDeg = [&](const remesh::CrossField& field) {
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
    };

    // Driven by parameter, not the environment: env-driven tests are order-dependent global state.
    const float off = creaseDeviationDeg(remesh::computeCrossField(mesh, 120, *backend, 0.0f));
    const float on = creaseDeviationDeg(remesh::computeCrossField(mesh, 120, *backend, 45.0f));

    CAPTURE(off);
    CAPTURE(on);
    CHECK(on < off);   // widening the alignment set must actually align the field to the crease
    CHECK(on < 5.0f);  // and on a single straight crease it should be nearly exact
}
