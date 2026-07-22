#include <doctest.h>

#include <array>
#include <string>
#include <vector>

#include "cyber/core/mesh.hpp"
#include "cyber/retopo/build_tools.hpp"
#include "cyber/retopo/commands.hpp"
#include "cyber/retopo/interactivity.hpp"

using cyber::EdgeId;
using cyber::FaceId;
using cyber::Index;
using cyber::Mesh;
using cyber::Vec3;
using cyber::VertexId;
namespace retopo = cyber::retopo;

namespace {

// Valid and every alive edge shared by at most two faces (2-manifold, boundary
// allowed).
bool isManifold(const Mesh& mesh) {
    if (!mesh.validate().empty()) {
        return false;
    }
    for (Index i = 0; i < mesh.edgeCapacity(); ++i) {
        const EdgeId e{i};
        if (mesh.isAlive(e) && mesh.edgeFaceCount(e) > 2) {
            return false;
        }
    }
    return true;
}

// A single flat quad in z = 0 spanning [-1,1] x [-1,1].
Mesh makeQuadMesh() {
    const std::vector<Vec3> p = {{-1, -1, 0}, {1, -1, 0}, {1, 1, 0}, {-1, 1, 0}};
    const std::vector<std::vector<Index>> f = {{0, 1, 2, 3}};
    return Mesh::fromIndexed(p, f);
}

}  // namespace

// ---- 9.3 advanced build tools ---------------------------------------------

TEST_CASE("buildQuad emits an nu x nv quad grid and stays manifold") {
    Mesh mesh;
    const retopo::GridPatch patch =
        retopo::buildQuad(mesh, {0, 0, 0}, {3, 0, 0}, {3, 2, 0}, {0, 2, 0}, 3, 2);
    CHECK(patch.faces.size() == 6);
    CHECK(patch.vertices.size() == 12);
    CHECK(mesh.faceCount() == 6);
    CHECK(isManifold(mesh));
}

TEST_CASE("buildTri emits two triangles per cell and stays manifold") {
    Mesh mesh;
    const retopo::GridPatch patch =
        retopo::buildTri(mesh, {0, 0, 0}, {2, 0, 0}, {2, 2, 0}, {0, 2, 0}, 2, 2);
    CHECK(patch.faces.size() == 8);  // 2x2 cells x 2 triangles
    CHECK(mesh.faceCount() == 8);
    CHECK(isManifold(mesh));
}

TEST_CASE("drawStrip builds a quad strip along a polyline and stays manifold") {
    Mesh mesh;
    const std::vector<Vec3> path = {{0, 0, 0}, {1, 0, 0}, {2, 0, 0}, {3, 0, 0}};
    const std::vector<FaceId> faces = retopo::drawStrip(mesh, path, {0, 1, 0});
    CHECK(faces.size() == 3);
    CHECK(mesh.faceCount() == 3);
    CHECK(isManifold(mesh));
}

TEST_CASE("extendBoundaryGrid extrudes a chain into rings of quads") {
    Mesh mesh;
    std::vector<VertexId> chain;
    for (int i = 0; i < 4; ++i) {
        chain.push_back(mesh.addVertex({static_cast<float>(i), 0, 0}));
    }
    const std::vector<FaceId> faces = retopo::extendBoundaryGrid(mesh, chain, {0, 1, 0}, 2);
    CHECK(faces.size() == 6);  // 3 segments x 2 rings
    CHECK(isManifold(mesh));
}

TEST_CASE("extendBoundaryFan closes a chain to an apex with triangles") {
    Mesh mesh;
    std::vector<VertexId> chain;
    for (int i = 0; i < 4; ++i) {
        chain.push_back(mesh.addVertex({static_cast<float>(i), 0, 0}));
    }
    const std::vector<FaceId> faces = retopo::extendBoundaryFan(mesh, chain, {0, 0, 1});
    CHECK(faces.size() == 3);
    CHECK(isManifold(mesh));
}

TEST_CASE("patchClone copies a face patch to a new location") {
    Mesh mesh = makeQuadMesh();
    const std::array<FaceId, 1> src = {FaceId{0}};
    const std::vector<FaceId> cloned = retopo::patchClone(mesh, src, Vec3{0, 0, 5});
    REQUIRE(cloned.size() == 1);
    CHECK(mesh.faceCount() == 2);
    CHECK(mesh.vertexCount() == 8);  // fresh disconnected copy
    // Cloned vertices are offset by +5 in z.
    const std::vector<VertexId> cv = mesh.faceVertices(cloned[0]);
    for (const VertexId v : cv) {
        CHECK(mesh.position(v).z == doctest::Approx(5.0f));
    }
    CHECK(isManifold(mesh));
}

TEST_CASE("transformVertices applies translate, scale and rotate") {
    Mesh mesh;
    const VertexId v = mesh.addVertex({1, 0, 0});
    const std::array<VertexId, 1> set = {v};

    retopo::transformVertices(mesh, set, retopo::Affine::translating({1, 2, 3}));
    CHECK(mesh.position(v).x == doctest::Approx(2.0f));
    CHECK(mesh.position(v).y == doctest::Approx(2.0f));
    CHECK(mesh.position(v).z == doctest::Approx(3.0f));

    mesh.setPosition(v, {2, 0, 0});
    retopo::transformVertices(mesh, set, retopo::Affine::scaling({3, 1, 1}));
    CHECK(mesh.position(v).x == doctest::Approx(6.0f));

    // Rotate (1,0,0) by 90 deg about +z -> (0,1,0).
    mesh.setPosition(v, {1, 0, 0});
    retopo::transformVertices(mesh, set, retopo::Affine::rotating({0, 0, 1}, cyber::kPi / 2.0f));
    CHECK(mesh.position(v).x == doctest::Approx(0.0f).epsilon(0.001));
    CHECK(mesh.position(v).y == doctest::Approx(1.0f));
}

TEST_CASE("pathDistribute spaces points evenly by arc length") {
    const std::vector<Vec3> path = {{0, 0, 0}, {2, 0, 0}, {4, 0, 0}};
    const std::vector<Vec3> pts = retopo::pathDistribute(path, 5);
    REQUIRE(pts.size() == 5);
    const std::array<float, 5> expected = {0.0f, 1.0f, 2.0f, 3.0f, 4.0f};
    for (std::size_t i = 0; i < pts.size(); ++i) {
        CHECK(pts[i].x == doctest::Approx(expected[i]));
        CHECK(pts[i].y == doctest::Approx(0.0f));
    }
}

TEST_CASE("surfaceCut splits faces along a plane and stays manifold") {
    Mesh mesh = makeQuadMesh();
    const retopo::Plane plane{{0, 0, 0}, {1, 0, 0}};  // x = 0
    const std::size_t splits = retopo::surfaceCut(mesh, plane);
    CHECK(splits == 1);
    CHECK(mesh.faceCount() == 2);
    CHECK(isManifold(mesh));
}

TEST_CASE("loopInfo reports edge count, length and closedness") {
    Mesh mesh;
    // A closed square loop of edges via a face so edgeBetween() exists.
    const FaceId f =
        mesh.addFace(std::array<VertexId, 4>{mesh.addVertex({0, 0, 0}), mesh.addVertex({2, 0, 0}),
                                             mesh.addVertex({2, 2, 0}), mesh.addVertex({0, 2, 0})});
    REQUIRE(f.valid());
    const std::vector<VertexId> loop = mesh.faceVertices(f);

    const retopo::LoopMetrics m = retopo::loopInfo(mesh, loop);
    CHECK(m.edgeCount == 4);
    CHECK(m.length == doctest::Approx(8.0f));  // 4 sides of length 2
    CHECK(m.closed);

    // Open sub-path: first three vertices (no closing edge listed).
    const std::vector<VertexId> open = {loop[0], loop[1], loop[2]};
    const retopo::LoopMetrics mo = retopo::loopInfo(mesh, open);
    CHECK(mo.edgeCount >= 2);
}

// ---- 9.4 pins, auto relax, whole-mesh commands, landmarks ------------------

TEST_CASE("autoRelax honors pins: a pinned interior vertex does not move") {
    Mesh mesh;
    const retopo::GridPatch patch =
        retopo::buildQuad(mesh, {0, 0, 0}, {2, 0, 0}, {2, 2, 0}, {0, 2, 0}, 2, 2);
    // Center vertex of a 2x2 grid: row 1, col 1 -> index 1*cols + 1.
    const VertexId center = patch.vertices[static_cast<std::size_t>(patch.cols + 1)];
    mesh.setPosition(center, {1, 1, 0.5f});  // perturb off-plane

    retopo::PinSet pins;
    pins.pin(center);
    retopo::autoRelax(mesh, pins, 4);
    CHECK(mesh.position(center).z == doctest::Approx(0.5f));  // pinned: unchanged
    CHECK(isManifold(mesh));
}

TEST_CASE("subdivideAll refines the whole mesh into quads") {
    Mesh mesh = makeQuadMesh();
    REQUIRE(mesh.faceCount() == 1);
    retopo::subdivideAll(mesh);
    CHECK(mesh.faceCount() == 4);  // one quad -> four
    CHECK(isManifold(mesh));
}

TEST_CASE("relaxAll smooths without pins and preserves manifoldness") {
    Mesh mesh;
    const retopo::GridPatch patch =
        retopo::buildQuad(mesh, {0, 0, 0}, {3, 0, 0}, {3, 3, 0}, {0, 3, 0}, 3, 3);
    const VertexId center = patch.vertices[static_cast<std::size_t>(2 * patch.cols + 2)];
    mesh.setPosition(center, {1.4f, 1.4f, 0.4f});
    retopo::relaxAll(mesh, nullptr, 3);
    CHECK(isManifold(mesh));
}

TEST_CASE("mirrorAll bakes symmetry into geometry") {
    Mesh mesh;
    const std::array<Vec3, 4> quad = {Vec3{0.1f, 0, 0}, Vec3{1, 0, 0}, Vec3{1, 1, 0},
                                      Vec3{0.1f, 1, 0}};
    REQUIRE(mesh.addFace(std::array<VertexId, 4>{mesh.addVertex(quad[0]), mesh.addVertex(quad[1]),
                                                 mesh.addVertex(quad[2]), mesh.addVertex(quad[3])})
                .valid());
    retopo::Symmetry sym;
    sym.plane = {{0, 0, 0}, {1, 0, 0}};
    const std::size_t added = retopo::mirrorAll(mesh, sym);
    CHECK(added == 1);
    CHECK(mesh.faceCount() == 2);
}

TEST_CASE("LandmarkLoops names, recalls and pins edge loops") {
    Mesh mesh;
    std::vector<VertexId> spine;
    for (int i = 0; i < 3; ++i) {
        spine.push_back(mesh.addVertex({0, static_cast<float>(i), 0}));
    }
    retopo::LandmarkLoops loops;
    loops.tag("spine", spine);
    loops.tag("belt", {spine[0], spine[1]});

    CHECK(loops.count() == 2);
    CHECK(loops.has("spine"));
    CHECK_FALSE(loops.has("unknown"));
    REQUIRE(loops.find("spine") != nullptr);
    CHECK(loops.find("spine")->size() == 3);
    CHECK(loops.names() == std::vector<std::string>{"belt", "spine"});  // sorted

    retopo::PinSet pins;
    loops.pinInto("spine", pins);
    CHECK(pins.count() == 3);
    loops.pinInto("unknown", pins);  // no-op
    CHECK(pins.count() == 3);

    loops.remove("belt");
    CHECK(loops.count() == 1);
}

// ---- 9.5 interactivity feedback, recorded traces, cost ---------------------

TEST_CASE("strokeFeedback flags recognized vs unrecognized strokes") {
    const std::vector<Vec3> square = {{0, 0, 0}, {1, 0, 0}, {1, 1, 0}, {0, 1, 0}, {0, 0.01f, 0}};
    const retopo::StrokeFeedback ok = retopo::strokeFeedback(square);
    CHECK(ok.kind == retopo::FeedbackKind::Recognized);
    CHECK(ok.action == retopo::StrokeAction::CreateQuad);

    // A two-point tap -> low-confidence Tweak -> unrecognized feedback with hint.
    const std::vector<Vec3> tap = {{0, 0, 0}, {0, 0, 0}};
    const retopo::StrokeFeedback bad = retopo::strokeFeedback(tap);
    CHECK(bad.kind == retopo::FeedbackKind::Unrecognized);
    CHECK(std::string(bad.hint).size() > 0);
}

TEST_CASE("recorded-trace table classifies to the expected actions") {
    const std::vector<retopo::RecordedTrace> traces = {
        {{{0, 0, 0}, {1, 0, 0}, {1, 1, 0}, {0, 1, 0}, {0, 0.01f, 0}},
         retopo::StrokeAction::CreateQuad},
        {{{0, 0, 0}, {2, 0, 0}, {1, 2, 0}, {0, 0.01f, 0}}, retopo::StrokeAction::CreateTri},
        {{{0, 0, 0}, {1, 0, 0}, {2, 0, 0}}, retopo::StrokeAction::Merge},
    };
    for (const retopo::RecordedTrace& t : traces) {
        CHECK(retopo::recognizeStroke(t.points).action == t.expected);
    }
}

TEST_CASE("estimateCost predicts operations per stroke") {
    const retopo::InteractivityCost quad =
        retopo::estimateCost({retopo::StrokeAction::CreateQuad, 0.8f}, 5);
    CHECK(quad.newVertices == 4);
    CHECK(quad.newFaces == 1);
    CHECK(quad.meshOps == 5);

    const retopo::InteractivityCost loop =
        retopo::estimateCost({retopo::StrokeAction::InsertLoop, 0.5f}, 6);
    CHECK(loop.meshOps == 3);

    // Cylinder cost scales with the ring (stroke point) count.
    const retopo::InteractivityCost cyl =
        retopo::estimateCost({retopo::StrokeAction::ExtrudeCylinder, 0.6f}, 8);
    CHECK(cyl.newFaces == 8);
    CHECK(cyl.newVertices == 8);
}
