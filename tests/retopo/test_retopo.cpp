#include <doctest.h>

#include <array>
#include <vector>

#include "cyber/core/mesh.hpp"
#include "cyber/retopo/actions.hpp"
#include "cyber/retopo/snapping.hpp"
#include "cyber/retopo/stroke_recognizer.hpp"
#include "cyber/retopo/symmetry.hpp"

using cyber::FaceId;
using cyber::Index;
using cyber::Mesh;
using cyber::Vec3;
using cyber::VertexId;
namespace retopo = cyber::retopo;

namespace {

// A flat quad in the z = 0 plane spanning [-1,1] x [-1,1], as a Target surface.
Mesh makePlaneMesh() {
    const std::vector<Vec3> p = {{-1, -1, 0}, {1, -1, 0}, {1, 1, 0}, {-1, 1, 0}};
    const std::vector<std::vector<Index>> f = {{0, 1, 2, 3}};
    return Mesh::fromIndexed(p, f);
}

// True when the mesh is valid and every alive edge is shared by at most two
// faces (2-manifold, allowing boundary).
bool isManifold(const Mesh& mesh) {
    if (!mesh.validate().empty()) {
        return false;
    }
    for (Index i = 0; i < mesh.edgeCapacity(); ++i) {
        const cyber::EdgeId e{i};
        if (mesh.isAlive(e) && mesh.edgeFaceCount(e) > 2) {
            return false;
        }
    }
    return true;
}

}  // namespace

TEST_CASE("projectToPlane drops a point orthogonally onto the plane") {
    const retopo::Plane plane{{0, 0, 0}, {0, 0, 1}};  // z = 0
    const Vec3 projected = retopo::projectToPlane(plane, {0.3f, 0.4f, 2.5f});
    CHECK(projected.x == doctest::Approx(0.3f));
    CHECK(projected.y == doctest::Approx(0.4f));
    CHECK(projected.z == doctest::Approx(0.0f));

    CHECK(retopo::signedDistance(plane, {0, 0, 3}) == doctest::Approx(3.0f));
    const Vec3 mirrored = retopo::mirrorAcrossPlane(plane, {0, 0, 2});
    CHECK(mirrored.z == doctest::Approx(-2.0f));
}

TEST_CASE("SurfaceSnapper snaps a point onto the Target surface and to a vertex") {
    const Mesh target = makePlaneMesh();
    const retopo::SurfaceSnapper snap(target);
    REQUIRE_FALSE(snap.empty());

    const retopo::SurfaceHit hit = snap.snapToSurface({0.25f, -0.5f, 1.7f});
    CHECK(hit.point.x == doctest::Approx(0.25f));
    CHECK(hit.point.y == doctest::Approx(-0.5f));
    CHECK(hit.point.z == doctest::Approx(0.0f));

    // Vertex-snap modifier: nearest existing Target vertex within radius.
    const auto vhit = snap.snapToVertex({0.9f, 0.9f, 0.2f}, 0.5f);
    REQUIRE(vhit.has_value());
    CHECK(vhit->point.x == doctest::Approx(1.0f));
    CHECK(vhit->point.y == doctest::Approx(1.0f));

    CHECK_FALSE(snap.snapToVertex({0.0f, 0.0f, 0.0f}, 0.1f).has_value());
}

TEST_CASE("Face create + extrude keeps the EditMesh manifold") {
    Mesh edit;
    const std::array<Vec3, 4> quad = {Vec3{0, 0, 0}, Vec3{1, 0, 0}, Vec3{1, 1, 0}, Vec3{0, 1, 0}};
    const FaceId face = retopo::createFace(edit, quad);
    REQUIRE(face.valid());
    REQUIRE(edit.faceCount() == 1);
    REQUIRE(isManifold(edit));

    // One mesh edit: extrude a quad from a boundary edge.
    const std::vector<VertexId> vs = edit.faceVertices(face);
    REQUIRE(vs.size() == 4);
    const cyber::EdgeId boundary = edit.edgeBetween(vs[1], vs[2]);
    REQUIRE(boundary.valid());

    const FaceId grown = retopo::extrudeEdge(edit, boundary, {2, 1, 0}, {2, 0, 0});
    REQUIRE(grown.valid());
    CHECK(edit.faceCount() == 2);
    // The shared edge is now interior (two faces); the mesh stays manifold.
    CHECK(edit.edgeFaceCount(boundary) == 2);
    CHECK(isManifold(edit));
}

TEST_CASE("insertLoop splits a quad into two quads and stays manifold") {
    Mesh edit;
    const std::array<Vec3, 4> quad = {Vec3{0, 0, 0}, Vec3{2, 0, 0}, Vec3{2, 1, 0}, Vec3{0, 1, 0}};
    const FaceId face = retopo::createFace(edit, quad);
    REQUIRE(face.valid());
    const std::vector<VertexId> vs = edit.faceVertices(face);
    const cyber::EdgeId across = edit.edgeBetween(vs[0], vs[1]);

    const FaceId newFace = retopo::insertLoop(edit, face, across);
    REQUIRE(newFace.valid());
    CHECK(edit.faceCount() == 2);
    CHECK(isManifold(edit));
}

TEST_CASE("recognizeStroke classifies canonical gestures") {
    // Roughly square closed stroke -> create quad.
    const std::vector<Vec3> square = {{0, 0, 0}, {1, 0, 0}, {1, 1, 0},
                                      {0, 1, 0}, {0, 0.01f, 0}};
    CHECK(retopo::recognizeStroke(square).action == retopo::StrokeAction::CreateQuad);

    // Straight line between two points -> merge.
    const std::vector<Vec3> line = {{0, 0, 0}, {1, 0, 0}, {2, 0, 0}};
    CHECK(retopo::recognizeStroke(line).action == retopo::StrokeAction::Merge);

    // Too few points -> tweak (tap-like).
    const std::vector<Vec3> tap = {{0, 0, 0}, {0, 0, 0}};
    CHECK(retopo::recognizeStroke(tap).action == retopo::StrokeAction::Tweak);
}

// Regression: a duplicated (coincident) sample makes one segment zero-length.
// Its normalized direction is {0,0,0}, so the old code scored acos(0)=90° as a
// spurious corner, turning a 3-sided closed stroke into a 4-sided one
// (CreateTri -> CreateQuad). Such junctions must be skipped.
TEST_CASE("recognizeStroke ignores zero-length segments from duplicate points") {
    // Closed triangle stroke: two interior corners -> three sides -> CreateTri.
    const std::vector<Vec3> tri = {{0, 0, 0}, {2, 0, 0}, {1, 1.5f, 0}, {0.01f, 0.01f, 0}};
    REQUIRE(retopo::recognizeStroke(tri).action == retopo::StrokeAction::CreateTri);

    // Same triangle with a corner sample duplicated: still a triangle, not a
    // quad, because the coincident point does not invent a corner.
    const std::vector<Vec3> triDup = {{0, 0, 0},      {2, 0, 0},        {2, 0, 0},
                                      {1, 1.5f, 0},   {0.01f, 0.01f, 0}};
    CHECK(retopo::recognizeStroke(triDup).action == retopo::StrokeAction::CreateTri);
}

TEST_CASE("applySymmetry mirrors working-side faces across the plane") {
    Mesh edit;
    // A quad entirely on the +x side of the x = 0 plane.
    const std::array<Vec3, 4> quad = {Vec3{0.1f, 0, 0}, Vec3{1, 0, 0}, Vec3{1, 1, 0},
                                      Vec3{0.1f, 1, 0}};
    REQUIRE(retopo::createFace(edit, quad).valid());

    retopo::Symmetry sym;
    sym.plane = {{0, 0, 0}, {1, 0, 0}};  // x = 0
    const std::size_t added = retopo::applySymmetry(edit, sym);
    CHECK(added == 1);
    CHECK(edit.faceCount() == 2);
}

// Regression: a face lying entirely on the mirror plane welds every vertex back
// onto itself, so mirroring it would re-add the same face with reversed winding
// -- a coincident, degenerate duplicate. Such faces must be skipped.
TEST_CASE("applySymmetry skips faces lying on the mirror plane") {
    Mesh edit;
    // A quad entirely on the x = 0 plane.
    const std::array<Vec3, 4> onPlane = {Vec3{0, 0, 0}, Vec3{0, 1, 0}, Vec3{0, 1, 1},
                                         Vec3{0, 0, 1}};
    REQUIRE(retopo::createFace(edit, onPlane).valid());

    retopo::Symmetry sym;
    sym.plane = {{0, 0, 0}, {1, 0, 0}};  // x = 0
    const std::size_t added = retopo::applySymmetry(edit, sym);
    CHECK(added == 0);
    CHECK(edit.faceCount() == 1);  // no degenerate duplicate created
}
