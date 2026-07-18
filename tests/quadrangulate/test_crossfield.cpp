#include <doctest.h>

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
    float theta = std::atan2(d.y, d.x);           // (-pi, pi]
    float m = std::fmod(std::fabs(theta), cyber::kPi / 2.0f);  // [0, pi/2)
    m = std::fmin(m, cyber::kPi / 2.0f - m);       // fold to nearest axis
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
