#include <doctest.h>

#include <cmath>
#include <limits>
#include <string>
#include <vector>

#include "cyber/core/mesh.hpp"
#include "cyber/retopo/loop_subdivide.hpp"

using cyber::EdgeId;
using cyber::FaceId;
using cyber::Index;
using cyber::Mesh;
using cyber::Vec3;
using cyber::VertexId;
namespace retopo = cyber::retopo;

namespace {

// Closed triangle mesh: a regular-ish tetrahedron. Every vertex is interior
// (valence 3), so the interior even-vertex mask is the only rule that applies.
Mesh makeTetrahedron() {
    const std::vector<Vec3> p = {{0, 0, 0}, {1, 0, 0}, {0, 1, 0}, {0, 0, 1}};
    const std::vector<std::vector<Index>> f = {{0, 2, 1}, {0, 1, 3}, {1, 2, 3}, {2, 0, 3}};
    return Mesh::fromIndexed(p, f);
}

// Open triangle mesh: a 3x3 grid of vertices over [-1,1]^2 split into 8
// triangles, with the centre vertex lifted to z = 1. The entire boundary lies
// exactly on z = 0, so any leak of the interior mask into the boundary rule
// shows up as a boundary vertex with z != 0.
Mesh makeTentGrid() {
    std::vector<Vec3> p;
    for (int j = 0; j < 3; ++j) {
        for (int i = 0; i < 3; ++i) {
            const bool centre = (i == 1 && j == 1);
            p.push_back(
                {static_cast<float>(i) - 1.0f, static_cast<float>(j) - 1.0f, centre ? 1.0f : 0.0f});
        }
    }
    std::vector<std::vector<Index>> f;
    for (Index j = 0; j < 2; ++j) {
        for (Index i = 0; i < 2; ++i) {
            const Index a = j * 3 + i;
            f.push_back({a, a + 1, a + 4});
            f.push_back({a, a + 4, a + 3});
        }
    }
    return Mesh::fromIndexed(p, f);
}

bool hasVertexAt(const Mesh& mesh, Vec3 p) {
    for (Index i = 0; i < mesh.vertexCapacity(); ++i) {
        const VertexId v{i};
        if (mesh.isAlive(v) && mesh.position(v) == p) {
            return true;
        }
    }
    return false;
}

// Distance from `p` to the nearest alive vertex of `mesh`. Comparing an
// original position against the refined mesh this way needs no id
// correspondence, which the rebuild deliberately does not promise.
float nearestVertexDistance(Vec3 p, const Mesh& mesh) {
    float nearest = std::numeric_limits<float>::max();
    for (Index i = 0; i < mesh.vertexCapacity(); ++i) {
        const VertexId v{i};
        if (mesh.isAlive(v)) {
            nearest = std::fmin(nearest, cyber::length(mesh.position(v) - p));
        }
    }
    return nearest;
}

}  // namespace

TEST_CASE("loopSubdivide splits every triangle into exactly four") {
    const Mesh tet = makeTetrahedron();
    REQUIRE(tet.faceCount() == 4);

    for (const retopo::LoopSubdivideMode mode :
         {retopo::LoopSubdivideMode::Smooth, retopo::LoopSubdivideMode::Linear}) {
        const retopo::LoopSubdivideResult once = retopo::loopSubdivide(tet, mode);
        REQUIRE(once.ok());
        CHECK(once.mesh.faceCount() == 16);
        // 4 originals + one point per edge (a tetrahedron has 6).
        CHECK(once.mesh.vertexCount() == 10);
        CHECK(once.mesh.validate().empty());
        for (Index i = 0; i < once.mesh.faceCapacity(); ++i) {
            const FaceId f{i};
            if (once.mesh.isAlive(f)) {
                CHECK(once.mesh.faceSize(f) == 3);
            }
        }

        // 4x compounds: the output is triangles, so it feeds back in.
        const retopo::LoopSubdivideResult twice = retopo::loopSubdivide(once.mesh, mode);
        REQUIRE(twice.ok());
        CHECK(twice.mesh.faceCount() == 64);
    }
}

TEST_CASE("linear mode adds triangles without moving anything") {
    const Mesh tet = makeTetrahedron();
    const retopo::LoopSubdivideResult res =
        retopo::loopSubdivide(tet, retopo::LoopSubdivideMode::Linear);
    REQUIRE(res.ok());

    // "More polygons, same shape": every original position survives bit-exact.
    for (Index i = 0; i < tet.vertexCapacity(); ++i) {
        const VertexId v{i};
        if (tet.isAlive(v)) {
            CHECK(hasVertexAt(res.mesh, tet.position(v)));
        }
    }
    // And every added vertex is an exact edge midpoint, so the refined surface
    // is the original surface — no vertex leaves the input's convex hull.
    for (Index i = 0; i < tet.edgeCapacity(); ++i) {
        const EdgeId e{i};
        if (tet.isAlive(e)) {
            const auto [a, b] = tet.edgeVertices(e);
            CHECK(hasVertexAt(res.mesh, cyber::lerp(tet.position(a), tet.position(b), 0.5f)));
        }
    }
}

TEST_CASE("smooth mode repositions vertices — the linear/smooth split is real") {
    const Mesh tet = makeTetrahedron();
    const retopo::LoopSubdivideResult smooth =
        retopo::loopSubdivide(tet, retopo::LoopSubdivideMode::Smooth);
    const retopo::LoopSubdivideResult linear =
        retopo::loopSubdivide(tet, retopo::LoopSubdivideMode::Linear);
    REQUIRE(smooth.ok());
    REQUIRE(linear.ok());

    // Same topology either way.
    CHECK(smooth.mesh.faceCount() == linear.mesh.faceCount());
    CHECK(smooth.mesh.vertexCount() == linear.mesh.vertexCount());

    // Linear leaves every original corner in place; smooth pulls all four
    // toward their one-ring, which is what a caller asking for "same shape"
    // must not get by accident. Valence 3 gives beta = 3/16, so the corners
    // move a long way — well past any float-noise threshold.
    for (Index i = 0; i < tet.vertexCapacity(); ++i) {
        const VertexId v{i};
        if (!tet.isAlive(v)) {
            continue;
        }
        CHECK(nearestVertexDistance(tet.position(v), linear.mesh) == doctest::Approx(0.0f));
        CHECK(nearestVertexDistance(tet.position(v), smooth.mesh) > 0.05f);
    }
}

TEST_CASE("smooth mode keeps boundary vertices on the boundary curve") {
    const Mesh grid = makeTentGrid();
    REQUIRE(grid.faceCount() == 8);
    const retopo::LoopSubdivideResult res =
        retopo::loopSubdivide(grid, retopo::LoopSubdivideMode::Smooth);
    REQUIRE(res.ok());
    CHECK(res.mesh.faceCount() == 32);

    // The whole input boundary lies on z = 0. The 1/8, 3/4, 1/8 curve rule
    // combines boundary positions only, so it cannot leave that plane; the
    // interior mask would blend in the z = 1 apex and lift the border.
    int boundaryVertices = 0;
    for (Index i = 0; i < res.mesh.edgeCapacity(); ++i) {
        const EdgeId e{i};
        if (!res.mesh.isAlive(e) || !res.mesh.isBoundaryEdge(e)) {
            continue;
        }
        const auto [a, b] = res.mesh.edgeVertices(e);
        boundaryVertices += 2;
        CHECK(res.mesh.position(a).z == doctest::Approx(0.0f));
        CHECK(res.mesh.position(b).z == doctest::Approx(0.0f));
    }
    // 8 boundary edges in, 16 out — a real boundary was inspected.
    CHECK(boundaryVertices == 32);

    // The apex is interior, so it does move: this is smooth mode after all.
    CHECK_FALSE(hasVertexAt(res.mesh, Vec3{0, 0, 1}));
}

TEST_CASE("a non-triangle input is refused by name, not silently triangulated") {
    Mesh quad;
    const VertexId a = quad.addVertex({0, 0, 0});
    const VertexId b = quad.addVertex({1, 0, 0});
    const VertexId c = quad.addVertex({1, 1, 0});
    const VertexId d = quad.addVertex({0, 1, 0});
    REQUIRE(quad.addFace(std::vector<VertexId>{a, b, c, d}).valid());

    const retopo::LoopSubdivideResult res =
        retopo::loopSubdivide(quad, retopo::LoopSubdivideMode::Smooth);
    CHECK_FALSE(res.ok());
    CHECK((res.error == retopo::LoopSubdivideError::NonTriangleFace));
    CHECK(res.offendingFace.valid());
    CHECK(res.offendingFaceSize == 4);
    CHECK(res.mesh.faceCount() == 0);  // nothing half-built comes back

    // The refusal survives a mesh that is mostly triangles: one quad anywhere
    // is enough, and the offending face is named.
    Mesh mixed = makeTetrahedron();
    const VertexId e = mixed.addVertex({2, 0, 0});
    const VertexId f = mixed.addVertex({3, 0, 0});
    const VertexId g = mixed.addVertex({3, 1, 0});
    const VertexId h = mixed.addVertex({2, 1, 0});
    const FaceId quadFace = mixed.addFace(std::vector<VertexId>{e, f, g, h});
    REQUIRE(quadFace.valid());
    const retopo::LoopSubdivideResult mixedRes =
        retopo::loopSubdivide(mixed, retopo::LoopSubdivideMode::Linear);
    CHECK((mixedRes.error == retopo::LoopSubdivideError::NonTriangleFace));
    CHECK((mixedRes.offendingFace == quadFace));

    // Mesh::triangulate() is the caller's explicit opt-in, and then it works.
    mixed.triangulate();
    const retopo::LoopSubdivideResult after =
        retopo::loopSubdivide(mixed, retopo::LoopSubdivideMode::Linear);
    CHECK(after.ok());
    CHECK(after.mesh.faceCount() == mixed.faceCount() * 4);
}

TEST_CASE("an empty mesh is refused by name") {
    const Mesh empty;
    const retopo::LoopSubdivideResult res =
        retopo::loopSubdivide(empty, retopo::LoopSubdivideMode::Smooth);
    CHECK_FALSE(res.ok());
    CHECK((res.error == retopo::LoopSubdivideError::EmptyMesh));
    // The typed code carries a message a host can show verbatim.
    CHECK(std::string(retopo::toString(res.error)) == "mesh has no faces");
}

// The three tests below pin the Loop masks to hand-computed values rather than
// to a qualitative property ("moved", "still on the border"). A weight that is
// wrong but still plausible — a midpoint where the mask belongs, a flat 1/16
// beta at every valence — satisfies the qualitative checks above and only shows
// up against the arithmetic itself.

TEST_CASE("the odd (edge) point uses Loop's 3/8-1/8 mask, not the midpoint") {
    // Tent grid edge v1=(0,-1,0) -- v4=(0,0,1), apexes v0=(-1,-1,0), v5=(1,0,0):
    //   3/8*(v1+v4) + 1/8*(v0+v5) = (0,-0.375,0.375) + (0,-0.125,0) = (0,-0.5,0.375)
    // The midpoint would be (0,-0.5,0.5); only the z coordinate separates them.
    const retopo::LoopSubdivideResult r =
        retopo::loopSubdivide(makeTentGrid(), retopo::LoopSubdivideMode::Smooth);
    REQUIRE(r.ok());
    const Mesh& out = r.mesh;
    CHECK(nearestVertexDistance({0.0f, -0.5f, 0.375f}, out) < 1e-5f);
    CHECK(nearestVertexDistance({0.0f, -0.5f, 0.5f}, out) > 1e-3f);
}

TEST_CASE("the boundary even point uses the 1/8-3/4-1/8 curve mask") {
    // Tent grid corner v0=(-1,-1,0) with boundary neighbours (0,-1,0), (-1,0,0):
    //   3/4*v0 + 1/8*[(0,-1,0)+(-1,0,0)] = (-0.75,-0.75,0) + (-0.125,-0.125,0)
    // A right-angle corner is the discriminating case: on a straight run the
    // curve mask and the neighbour midpoint agree, so a flat border proves nothing.
    const retopo::LoopSubdivideResult r =
        retopo::loopSubdivide(makeTentGrid(), retopo::LoopSubdivideMode::Smooth);
    REQUIRE(r.ok());
    const Mesh& out = r.mesh;
    CHECK(nearestVertexDistance({-0.875f, -0.875f, 0.0f}, out) < 1e-5f);
    CHECK(nearestVertexDistance({-0.5f, -0.5f, 0.0f}, out) > 1e-3f);
}

TEST_CASE("the interior even mask is valence-dependent, not a flat 1/16") {
    // Tetrahedron: every vertex is interior at valence 3, where Loop's beta is
    //   (1/3)(5/8 - (3/8 + 1/4*cos(2pi/3))^2) = (1/3)(0.625 - 0.0625) = 0.1875,
    // three times the regular-valence 1/16. For v0=(0,0,0) with ring sum (1,1,1):
    //   v0*(1 - 3*0.1875) + (1,1,1)*0.1875 = (0.1875, 0.1875, 0.1875)
    // whereas a hardcoded 1/16 would place it at (0.0625, 0.0625, 0.0625).
    const retopo::LoopSubdivideResult r =
        retopo::loopSubdivide(makeTetrahedron(), retopo::LoopSubdivideMode::Smooth);
    REQUIRE(r.ok());
    const Mesh& out = r.mesh;
    CHECK(nearestVertexDistance({0.1875f, 0.1875f, 0.1875f}, out) < 1e-5f);
    CHECK(nearestVertexDistance({0.0625f, 0.0625f, 0.0625f}, out) > 1e-3f);
}
