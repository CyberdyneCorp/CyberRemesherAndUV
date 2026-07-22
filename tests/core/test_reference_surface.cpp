#include <doctest.h>

#include <cmath>
#include <vector>

#include "cyber/core/mesh.hpp"
#include "cyber/core/reference_surface.hpp"

using cyber::Index;
using cyber::Mesh;
using cyber::Vec3;
using cyber::VertexId;
namespace remesh = cyber::remesh;

namespace {

// Two triangles covering the unit square in the z = 0 plane.
Mesh makeFlatQuad() {
    const std::vector<Vec3> p = {{0, 0, 0}, {1, 0, 0}, {1, 1, 0}, {0, 1, 0}};
    const std::vector<std::vector<Index>> f = {{0, 1, 2}, {0, 2, 3}};
    return Mesh::fromIndexed(p, f);
}

// Triangulated UV sphere of radius 1 (its facets are flat chords strictly
// inside the sphere, which is what the curved projection has to recover).
Mesh makeSphere(int rings, int segments) {
    std::vector<Vec3> p;
    p.push_back({0, 0, 1});
    for (int r = 1; r < rings; ++r) {
        const float phi = cyber::kPi * static_cast<float>(r) / static_cast<float>(rings);
        for (int s = 0; s < segments; ++s) {
            const float theta =
                2.0f * cyber::kPi * static_cast<float>(s) / static_cast<float>(segments);
            p.push_back(
                {std::sin(phi) * std::cos(theta), std::sin(phi) * std::sin(theta), std::cos(phi)});
        }
    }
    p.push_back({0, 0, -1});
    const Index south = static_cast<Index>(p.size() - 1);

    std::vector<std::vector<Index>> f;
    auto ringVertex = [segments](int r, int s) {
        return static_cast<Index>(1 + (r - 1) * segments + (s % segments));
    };
    for (int s = 0; s < segments; ++s) {
        f.push_back({0, ringVertex(1, s), ringVertex(1, s + 1)});
    }
    for (int r = 1; r + 1 < rings; ++r) {
        for (int s = 0; s < segments; ++s) {
            const Index a = ringVertex(r, s), b = ringVertex(r, s + 1);
            const Index c = ringVertex(r + 1, s), d = ringVertex(r + 1, s + 1);
            f.push_back({a, c, d});
            f.push_back({a, d, b});
        }
    }
    for (int s = 0; s < segments; ++s) {
        f.push_back({south, ringVertex(rings - 1, s + 1), ringVertex(rings - 1, s)});
    }
    return Mesh::fromIndexed(p, f);
}

}  // namespace

TEST_CASE("empty reference surface reports empty and projects to itself") {
    const remesh::ReferenceSurface surface;
    REQUIRE(surface.empty());
}

TEST_CASE("zero smoothing degrades to flat closest-point projection") {
    Mesh mesh = makeFlatQuad();
    const remesh::ReferenceSurface surface(mesh, 0.0f);
    REQUIRE_FALSE(surface.empty());

    // A point above the quad drops straight to the plane; a point off the side
    // clamps to the nearest edge — plain closest-point behaviour.
    const Vec3 inside = surface.project({0.3f, 0.4f, 2.5f});
    REQUIRE(inside.x == doctest::Approx(0.3f));
    REQUIRE(inside.y == doctest::Approx(0.4f));
    REQUIRE(inside.z == doctest::Approx(0.0f));

    const Vec3 offEdge = surface.project({1.7f, 0.5f, 0.0f});
    REQUIRE(offEdge.x == doctest::Approx(1.0f));
    REQUIRE(offEdge.z == doctest::Approx(0.0f));
}

TEST_CASE("PN projection is exact on a flat mesh (degenerate Bezier stays planar)") {
    // Even with maximum smoothing, a planar surface has all corner normals
    // parallel, so every PN control point stays in the plane and the patch is
    // flat. This pins the Bezier evaluation against drift.
    Mesh mesh = makeFlatQuad();
    const remesh::ReferenceSurface surface(mesh, 180.0f);

    for (const Vec3 q :
         {Vec3{0.25f, 0.25f, 3.0f}, Vec3{0.6f, 0.7f, -1.0f}, Vec3{0.5f, 0.5f, 5.0f}}) {
        const Vec3 p = surface.project(q);
        REQUIRE(p.x == doctest::Approx(q.x));
        REQUIRE(p.y == doctest::Approx(q.y));
        REQUIRE(p.z == doctest::Approx(0.0f));
    }
}

TEST_CASE("PN projection bulges a coarse sphere toward its true surface") {
    // Sample points just outside each facet midpoint. Flat projection returns
    // the facet (radius < 1); PN projection reconstructs the curvature and
    // lands measurably closer to the unit sphere.
    Mesh mesh = makeSphere(6, 10);
    const remesh::ReferenceSurface flat(mesh, 0.0f);
    const remesh::ReferenceSurface smooth(mesh, 180.0f);

    double flatError = 0.0;
    double smoothError = 0.0;
    std::size_t samples = 0;
    for (Index fi = 0; fi < mesh.faceCapacity(); ++fi) {
        const cyber::FaceId f{fi};
        if (!mesh.isAlive(f)) {
            continue;
        }
        const auto verts = mesh.faceVertices(f);
        const Vec3 centroid =
            (mesh.position(verts[0]) + mesh.position(verts[1]) + mesh.position(verts[2])) / 3.0f;
        // Query from a little outside so the closest point is this facet.
        const Vec3 query = centroid * 1.1f;
        flatError += std::fabs(length(flat.project(query)) - 1.0f);
        smoothError += std::fabs(length(smooth.project(query)) - 1.0f);
        ++samples;
    }
    REQUIRE(samples > 0);
    flatError /= static_cast<double>(samples);
    smoothError /= static_cast<double>(samples);

    // The flat facets sit well inside the sphere; the curved patch cuts that
    // error by more than half.
    REQUIRE(flatError > 0.01);
    REQUIRE(smoothError < flatError * 0.5);
}

TEST_CASE("the smoothing angle gates whether a ridge is creased or blended") {
    // A roof: two flat slopes meeting along a sharp ridge (edge 2-3). Below
    // the ridge dihedral the ridge is a crease — corner normals are not
    // averaged across it, so each slope projects flat. Above it the normals
    // blend and the patch curves. Comparing both against the flat baseline
    // isolates the effect without depending on the winding-derived sign.
    const std::vector<Vec3> p = {{0, 0, 0}, {2, 0, 0},  {2, 0, 1},
                                 {0, 0, 1}, {2, -1, 2}, {0, -1, 2}};
    const std::vector<std::vector<Index>> f = {{0, 1, 2}, {0, 2, 3}, {3, 2, 4}, {3, 4, 5}};
    Mesh mesh = Mesh::fromIndexed(p, f);

    const remesh::ReferenceSurface flat(mesh, 0.0f);
    const remesh::ReferenceSurface crease(mesh, 10.0f);  // ridge far exceeds 10 deg -> crease kept
    const remesh::ReferenceSurface blended(mesh, 179.0f);  // everything smooths together

    const Vec3 q{1.0f, 1.0f, 0.5f};
    const Vec3 base = flat.project(q);
    auto deviation = [&](const remesh::ReferenceSurface& s) { return length(s.project(q) - base); };

    // Crease preserved: identical to the flat closest point.
    REQUIRE(deviation(crease) < 1e-4f);
    // Blending across the ridge bends the patch away from the flat facet.
    REQUIRE(deviation(blended) > deviation(crease));
}
