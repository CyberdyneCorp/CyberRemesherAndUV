#include <doctest.h>

#include <cmath>
#include <vector>

#include "cyber/core/mesh.hpp"
#include "cyber/core/plane.hpp"
#include "cyber/quadrangulate/symmetry_layout.hpp"

using cyber::FaceId;
using cyber::Index;
using cyber::Mesh;
using cyber::Plane;
using cyber::Vec3;
using cyber::VertexId;
using cyber::remesh::isTopologicallySymmetric;
using cyber::remesh::mirrorAcross;
using cyber::remesh::splitAtPlane;
using cyber::remesh::SymmetryAxis;
using cyber::remesh::symmetryPlane;

namespace {

// A quad grid spanning x in [-n, n], y in [0, n], so the x = 0 plane cuts it in
// half through a column of vertices.
Mesh centredGrid(int n) {
    std::vector<Vec3> p;
    for (int y = 0; y <= n; ++y) {
        for (int x = -n; x <= n; ++x) {
            p.push_back(Vec3{static_cast<float>(x), static_cast<float>(y), 0.0f});
        }
    }
    const int w = 2 * n + 1;
    const auto at = [w, n](int x, int y) { return static_cast<Index>(y * w + (x + n)); };
    std::vector<std::vector<Index>> f;
    for (int y = 0; y < n; ++y) {
        for (int x = -n; x < n; ++x) {
            f.push_back({at(x, y), at(x + 1, y), at(x + 1, y + 1), at(x, y + 1)});
        }
    }
    return Mesh::fromIndexed(p, f);
}

// The same grid shifted by half a cell, so the plane cuts THROUGH faces rather
// than along a column of vertices — the case that needs real clipping.
Mesh offsetGrid(int n) {
    std::vector<Vec3> p;
    for (int y = 0; y <= n; ++y) {
        for (int x = -n; x <= n; ++x) {
            p.push_back(Vec3{static_cast<float>(x) + 0.5f, static_cast<float>(y), 0.0f});
        }
    }
    const int w = 2 * n + 1;
    const auto at = [w, n](int x, int y) { return static_cast<Index>(y * w + (x + n)); };
    std::vector<std::vector<Index>> f;
    for (int y = 0; y < n; ++y) {
        for (int x = -n; x < n; ++x) {
            f.push_back({at(x, y), at(x + 1, y), at(x + 1, y + 1), at(x, y + 1)});
        }
    }
    return Mesh::fromIndexed(p, f);
}

std::size_t aliveFaces(const Mesh& mesh) {
    std::size_t n = 0;
    for (Index f = 0; f < mesh.faceCapacity(); ++f) {
        n += mesh.isAlive(FaceId{f}) ? 1u : 0u;
    }
    return n;
}

std::size_t aliveVertices(const Mesh& mesh) {
    std::size_t n = 0;
    for (Index v = 0; v < mesh.vertexCapacity(); ++v) {
        n += mesh.isAlive(VertexId{v}) ? 1u : 0u;
    }
    return n;
}

Plane planeX() { return Plane{Vec3{0, 0, 0}, Vec3{1, 0, 0}}; }

}  // namespace

TEST_CASE("symmetry plane runs through the bounding-box centre") {
    const Mesh mesh = centredGrid(3);
    const Plane p = symmetryPlane(mesh, SymmetryAxis::X);
    CHECK(p.point.x == doctest::Approx(0.0f));
    CHECK(p.normal.x == doctest::Approx(1.0f));
    // None yields a zero normal, which every operation reads as "no symmetry".
    CHECK(cyber::length(symmetryPlane(mesh, SymmetryAxis::None).normal) == doctest::Approx(0.0f));
}

TEST_CASE("splitting keeps one side and puts the border exactly on the plane") {
    const Mesh mesh = centredGrid(3);
    const auto split = splitAtPlane(mesh, planeX(), /*positiveSide=*/true);
    REQUIRE(split.valid);
    CHECK(aliveFaces(split.half) == aliveFaces(mesh) / 2);
    CHECK_FALSE(split.centerline.empty());
    for (Index v = 0; v < split.half.vertexCapacity(); ++v) {
        const VertexId vid{v};
        if (split.half.isAlive(vid)) {
            CHECK(split.half.position(vid).x >= -1e-6f);
        }
    }
    // "Exactly on", not "nearly on": a border that only nearly reaches the plane
    // would leave a visible seam once mirrored.
    for (const VertexId v : split.centerline) {
        CHECK(split.half.position(v).x == 0.0f);
    }
}

TEST_CASE("splitting clips faces the plane passes through") {
    // The offset grid has no vertex column on the plane, so the split must cut
    // faces and create new border vertices rather than just dropping faces.
    const Mesh mesh = offsetGrid(3);
    const auto split = splitAtPlane(mesh, planeX(), /*positiveSide=*/true);
    REQUIRE(split.valid);
    REQUIRE_FALSE(split.centerline.empty());
    for (const VertexId v : split.centerline) {
        CHECK(split.half.position(v).x == 0.0f);
    }
    // Every kept vertex is on the working side.
    for (Index v = 0; v < split.half.vertexCapacity(); ++v) {
        const VertexId vid{v};
        if (split.half.isAlive(vid)) {
            CHECK(split.half.position(vid).x >= -1e-6f);
        }
    }
}

TEST_CASE("mirroring produces EXACT topological symmetry, not approximate shape") {
    // The property retopology actually needs: reflecting the result reproduces
    // it vertex for vertex and face for face. Solving a whole mesh and hoping
    // the numerics stay symmetric gives matching SHAPE at best, with the two
    // halves carrying different edge counts.
    const Mesh mesh = centredGrid(3);
    const auto split = splitAtPlane(mesh, planeX(), true);
    REQUIRE(split.valid);

    Mesh whole = split.half;
    const std::size_t halfFaces = aliveFaces(whole);
    const auto report = mirrorAcross(whole, planeX());
    REQUIRE(report.valid);
    CHECK(report.mirroredFaces == halfFaces);
    CHECK(aliveFaces(whole) == 2 * halfFaces);
    CHECK(isTopologicallySymmetric(whole, planeX()));
}

TEST_CASE("the centerline is welded, not duplicated") {
    // Duplicating the on-plane vertices would leave two coincident borders that
    // look right and are not connected — a crack down the middle of the model.
    const Mesh mesh = centredGrid(3);
    const auto split = splitAtPlane(mesh, planeX(), true);
    REQUIRE(split.valid);

    Mesh whole = split.half;
    const std::size_t halfVerts = aliveVertices(whole);
    const std::size_t centerline = split.centerline.size();
    const auto report = mirrorAcross(whole, planeX());
    REQUIRE(report.valid);

    // Every off-plane vertex gained a twin; the centerline gained none.
    CHECK(report.mirroredVertices == halfVerts - centerline);
    CHECK(aliveVertices(whole) == 2 * halfVerts - centerline);

    // And the seam is manifold: no centerline edge bounds only one face.
    for (Index e = 0; e < whole.edgeCapacity(); ++e) {
        const cyber::EdgeId edge{e};
        if (!whole.isAlive(edge)) {
            continue;
        }
        const auto [a, b] = whole.edgeVertices(edge);
        if (whole.position(a).x == 0.0f && whole.position(b).x == 0.0f) {
            CHECK(whole.edgeFaceCount(edge) == 2);
        }
    }
}

TEST_CASE("an asymmetric mesh is reported asymmetric") {
    // The check has to be able to FAIL, or it proves nothing about the meshes it
    // passes. Two negative cases, because they fail for different reasons.

    SUBCASE("a half with nothing mirroring it") {
        // The bare half has no partner for any off-plane vertex.
        const Mesh mesh = centredGrid(3);
        const auto split = splitAtPlane(mesh, planeX(), true);
        REQUIRE(split.valid);
        CHECK_FALSE(isTopologicallySymmetric(split.half, planeX()));
    }

    SUBCASE("one vertex nudged off its mirror position") {
        const Mesh mesh = centredGrid(3);
        const auto split = splitAtPlane(mesh, planeX(), true);
        Mesh whole = split.half;
        mirrorAcross(whole, planeX());
        REQUIRE(isTopologicallySymmetric(whole, planeX()));

        for (Index v = 0; v < whole.vertexCapacity(); ++v) {
            const VertexId vid{v};
            if (whole.isAlive(vid) && whole.position(vid).x > 0.5f) {
                whole.setPosition(vid, whole.position(vid) + Vec3{0.37f, 0.0f, 0.0f});
                break;
            }
        }
        CHECK_FALSE(isTopologicallySymmetric(whole, planeX()));
    }
}

TEST_CASE("a mesh that is already symmetric is recognised as such") {
    // The centred grid spans x symmetrically, so it passes on its own — which
    // is worth pinning, because a checker that only ever says "no" would make
    // the negative cases above meaningless.
    const Mesh mesh = centredGrid(3);
    CHECK(isTopologicallySymmetric(mesh, planeX()));
}

TEST_CASE("mirroring is deterministic") {
    const Mesh mesh = centredGrid(4);
    const auto split = splitAtPlane(mesh, planeX(), true);
    REQUIRE(split.valid);
    Mesh first = split.half;
    const auto reportFirst = mirrorAcross(first, planeX());
    for (int i = 0; i < 3; ++i) {
        Mesh again = split.half;
        const auto report = mirrorAcross(again, planeX());
        CHECK(report.mirroredVertices == reportFirst.mirroredVertices);
        CHECK(report.mirroredFaces == reportFirst.mirroredFaces);
        CHECK(report.weldedCenterline == reportFirst.weldedCenterline);
        CHECK(aliveVertices(again) == aliveVertices(first));
        CHECK(aliveFaces(again) == aliveFaces(first));
    }
}

TEST_CASE("no symmetry axis is a no-op, not a crash") {
    Mesh mesh = centredGrid(2);
    const std::size_t faces = aliveFaces(mesh);
    const Plane none = symmetryPlane(mesh, SymmetryAxis::None);
    CHECK_FALSE(splitAtPlane(mesh, none, true).valid);
    CHECK_FALSE(mirrorAcross(mesh, none).valid);
    CHECK(aliveFaces(mesh) == faces);
    CHECK_FALSE(isTopologicallySymmetric(mesh, none));
}
