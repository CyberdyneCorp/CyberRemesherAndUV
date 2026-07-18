#include <doctest.h>

#include <array>
#include <vector>

#include "cyber/core/mesh.hpp"

using cyber::FaceId;
using cyber::Index;
using cyber::Mesh;
using cyber::Vec3;
using cyber::VertexId;

namespace {

Mesh makeQuadAndTri() {
    // Two faces sharing an edge: quad (0,1,2,3) + triangle (1,4,2).
    const std::vector<Vec3> positions = {
        {0, 0, 0}, {1, 0, 0}, {1, 1, 0}, {0, 1, 0}, {2, 0.5f, 0},
    };
    const std::vector<std::vector<Index>> faces = {{0, 1, 2, 3}, {1, 4, 2}};
    return Mesh::fromIndexed(positions, faces);
}

Mesh makeCube() {
    const std::vector<Vec3> p = {
        {0, 0, 0}, {1, 0, 0}, {1, 1, 0}, {0, 1, 0}, {0, 0, 1}, {1, 0, 1}, {1, 1, 1}, {0, 1, 1},
    };
    const std::vector<std::vector<Index>> f = {
        {0, 3, 2, 1}, {4, 5, 6, 7}, {0, 1, 5, 4}, {1, 2, 6, 5}, {2, 3, 7, 6}, {3, 0, 4, 7},
    };
    return Mesh::fromIndexed(p, f);
}

void requireValid(const Mesh& mesh) {
    const auto errors = mesh.validate();
    for (const auto& e : errors) {
        CAPTURE(e);
    }
    REQUIRE(errors.empty());
}

}  // namespace

TEST_CASE("indexed round-trip preserves mixed tri/quad faces") {
    Mesh mesh = makeQuadAndTri();
    requireValid(mesh);
    REQUIRE(mesh.vertexCount() == 5);
    REQUIRE(mesh.faceCount() == 2);
    REQUIRE(mesh.edgeCount() == 6);  // 4 quad + 3 tri - 1 shared

    std::vector<Vec3> positions;
    std::vector<std::vector<Index>> faces;
    mesh.toIndexed(positions, faces);
    REQUIRE(positions.size() == 5);
    REQUIRE(faces.size() == 2);
    REQUIRE(faces[0] == std::vector<Index>{0, 1, 2, 3});
    REQUIRE(faces[1] == std::vector<Index>{1, 4, 2});
}

TEST_CASE("degenerate faces are rejected") {
    Mesh mesh;
    const VertexId a = mesh.addVertex({0, 0, 0});
    const VertexId b = mesh.addVertex({1, 0, 0});
    const VertexId c = mesh.addVertex({0, 1, 0});
    REQUIRE(!mesh.addFace(std::array{a, b}).valid());
    REQUIRE(!mesh.addFace(std::array{a, b, a}).valid());
    REQUIRE(mesh.addFace(std::array{a, b, c}).valid());
    requireValid(mesh);
}

TEST_CASE("non-manifold edge keeps all three faces (spec: mesh-core)") {
    // Three triangles fanning around one shared edge (0,1).
    const std::vector<Vec3> p = {{0, 0, 0}, {0, 0, 1}, {1, 0, 0}, {0, 1, 0}, {-1, -1, 0}};
    const std::vector<std::vector<Index>> f = {{0, 1, 2}, {0, 1, 3}, {0, 1, 4}};
    Mesh mesh = Mesh::fromIndexed(p, f);
    requireValid(mesh);
    REQUIRE(mesh.faceCount() == 3);

    const auto edge = mesh.edgeBetween(VertexId{0}, VertexId{1});
    REQUIRE(edge.valid());
    REQUIRE(mesh.edgeFaceCount(edge) == 3);

    const auto islands = mesh.islands();
    REQUIRE(islands.size() == 1);
    REQUIRE(islands[0].size() == 3);  // all faces present, none dropped
}

TEST_CASE("island detection separates disconnected shells deterministically") {
    const std::vector<Vec3> p = {{0, 0, 0}, {1, 0, 0}, {0, 1, 0}, {5, 0, 0}, {6, 0, 0}, {5, 1, 0}};
    const std::vector<std::vector<Index>> f = {{0, 1, 2}, {3, 4, 5}};
    Mesh mesh = Mesh::fromIndexed(p, f);
    const auto islands = mesh.islands();
    REQUIRE(islands.size() == 2);
    REQUIRE(islands[0][0].value < islands[1][0].value);
}

TEST_CASE("feature tagging marks exactly the 12 cube edges (spec: mesh-core)") {
    Mesh cube = makeCube();
    requireValid(cube);
    cube.tagFeatureEdges(90.0f);
    std::size_t tagged = 0;
    for (Index i = 0; i < cube.edgeCapacity(); ++i) {
        if (cube.isAlive(cyber::EdgeId{i}) && cube.isFeatureEdge(cyber::EdgeId{i})) {
            ++tagged;
        }
    }
    REQUIRE(tagged == 12);
}

TEST_CASE("flat plane has no feature edges at 90 degrees") {
    Mesh mesh = makeQuadAndTri();
    mesh.tagFeatureEdges(90.0f);
    // Interior shared edge is flat -> not a feature; boundary edges are.
    const auto shared = mesh.edgeBetween(VertexId{1}, VertexId{2});
    REQUIRE(!mesh.isFeatureEdge(shared));
}

TEST_CASE("splitEdge inserts a vertex into edge and faces") {
    Mesh mesh = makeQuadAndTri();
    const auto shared = mesh.edgeBetween(VertexId{1}, VertexId{2});
    const VertexId m = mesh.splitEdge(shared, 0.5f);
    REQUIRE(m.valid());
    requireValid(mesh);
    REQUIRE(mesh.vertexCount() == 6);
    REQUIRE(mesh.faceCount() == 2);
    // Quad became a pentagon, triangle became a quad.
    std::size_t five = 0, four = 0;
    for (Index i = 0; i < mesh.faceCapacity(); ++i) {
        if (!mesh.isAlive(FaceId{i})) {
            continue;
        }
        five += mesh.faceSize(FaceId{i}) == 5 ? 1u : 0u;
        four += mesh.faceSize(FaceId{i}) == 4 ? 1u : 0u;
    }
    REQUIRE(five == 1);
    REQUIRE(four == 1);
    const Vec3 pm = mesh.position(m);
    REQUIRE(pm.x == doctest::Approx(1.0f));
    REQUIRE(pm.y == doctest::Approx(0.5f));
}

TEST_CASE("splitEdge propagates the feature flag to both halves") {
    Mesh mesh = makeQuadAndTri();
    const auto shared = mesh.edgeBetween(VertexId{1}, VertexId{2});
    mesh.setFeatureEdge(shared, true);
    const VertexId m = mesh.splitEdge(shared, 0.5f);
    REQUIRE(mesh.isFeatureEdge(mesh.edgeBetween(VertexId{1}, m)));
    REQUIRE(mesh.isFeatureEdge(mesh.edgeBetween(m, VertexId{2})));
}

TEST_CASE("splitFace divides a quad into two triangles") {
    Mesh mesh = makeQuadAndTri();
    FaceId quad{};
    for (Index i = 0; i < mesh.faceCapacity(); ++i) {
        if (mesh.isAlive(FaceId{i}) && mesh.faceSize(FaceId{i}) == 4) {
            quad = FaceId{i};
        }
    }
    const FaceId other = mesh.splitFace(quad, VertexId{0}, VertexId{2});
    REQUIRE(other.valid());
    requireValid(mesh);
    REQUIRE(mesh.faceSize(quad) == 3);
    REQUIRE(mesh.faceSize(other) == 3);
    REQUIRE(mesh.faceCount() == 3);
}

TEST_CASE("triangulate fans every n-gon") {
    Mesh cube = makeCube();
    cube.triangulate();
    requireValid(cube);
    REQUIRE(cube.faceCount() == 12);
    for (Index i = 0; i < cube.faceCapacity(); ++i) {
        if (cube.isAlive(FaceId{i})) {
            REQUIRE(cube.faceSize(FaceId{i}) == 3);
        }
    }
}

TEST_CASE("flipEdge rewires a triangle pair") {
    const std::vector<Vec3> p = {{0, 0, 0}, {1, 0, 0}, {1, 1, 0}, {0, 1, 0}};
    const std::vector<std::vector<Index>> f = {{0, 1, 2}, {0, 2, 3}};
    Mesh mesh = Mesh::fromIndexed(p, f);
    const auto diagonal = mesh.edgeBetween(VertexId{0}, VertexId{2});
    REQUIRE(mesh.flipEdge(diagonal));
    requireValid(mesh);
    REQUIRE(!mesh.edgeBetween(VertexId{0}, VertexId{2}).valid());
    REQUIRE(mesh.edgeBetween(VertexId{1}, VertexId{3}).valid());
    REQUIRE(mesh.faceCount() == 2);
    // Flipping back restores the original diagonal.
    REQUIRE(mesh.flipEdge(mesh.edgeBetween(VertexId{1}, VertexId{3})));
    REQUIRE(mesh.edgeBetween(VertexId{0}, VertexId{2}).valid());
}

TEST_CASE("flipEdge refuses boundary and non-triangle edges") {
    Mesh mesh = makeQuadAndTri();
    const auto boundary = mesh.edgeBetween(VertexId{0}, VertexId{1});
    REQUIRE(!mesh.flipEdge(boundary));
    const auto shared = mesh.edgeBetween(VertexId{1}, VertexId{2});
    REQUIRE(!mesh.flipEdge(shared));  // quad on one side
}

TEST_CASE("collapseEdge merges endpoints and drops degenerate faces") {
    const std::vector<Vec3> p = {{0, 0, 0}, {1, 0, 0}, {1, 1, 0}, {0, 1, 0}};
    const std::vector<std::vector<Index>> f = {{0, 1, 2}, {0, 2, 3}};
    Mesh mesh = Mesh::fromIndexed(p, f);
    const auto diagonal = mesh.edgeBetween(VertexId{0}, VertexId{2});
    const VertexId kept = mesh.edgeVertices(diagonal).first;
    REQUIRE(mesh.collapseEdge(diagonal));
    requireValid(mesh);
    REQUIRE(mesh.faceCount() == 0);  // both triangles degenerate
    // Midpoint placement on the surviving endpoint.
    REQUIRE(mesh.isAlive(kept));
    REQUIRE(mesh.position(kept).x == doctest::Approx(0.5f));
    REQUIRE(mesh.position(kept).y == doctest::Approx(0.5f));
}

TEST_CASE("collapseEdge on a larger patch keeps surrounding faces valid") {
    // 3x3 vertex grid of quads.
    std::vector<Vec3> p;
    for (int y = 0; y < 3; ++y) {
        for (int x = 0; x < 3; ++x) {
            p.push_back({static_cast<float>(x), static_cast<float>(y), 0});
        }
    }
    std::vector<std::vector<Index>> f;
    for (Index y = 0; y < 2; ++y) {
        for (Index x = 0; x < 2; ++x) {
            const Index i = y * 3 + x;
            f.push_back({i, i + 1, i + 4, i + 3});
        }
    }
    Mesh mesh = Mesh::fromIndexed(p, f);
    requireValid(mesh);
    REQUIRE(mesh.faceCount() == 4);
    const auto e = mesh.edgeBetween(VertexId{1}, VertexId{4});  // interior edge
    REQUIRE(mesh.collapseEdge(e));
    requireValid(mesh);
    // The two quads flanking the collapsed edge become triangles.
    REQUIRE(mesh.faceCount() == 4);
    std::size_t tris = 0;
    for (Index i = 0; i < mesh.faceCapacity(); ++i) {
        if (mesh.isAlive(FaceId{i}) && mesh.faceSize(FaceId{i}) == 3) {
            ++tris;
        }
    }
    REQUIRE(tris == 2);
}

TEST_CASE("vertex colors survive subdivision (spec: mesh-core)") {
    Mesh mesh = makeQuadAndTri();
    auto& colors = mesh.vertexAttributes().create<Vec3>("color");
    for (std::size_t i = 0; i < colors.size(); ++i) {
        colors[i] = {1.0f, 0.0f, 0.0f};
    }
    Mesh fine = mesh.linearSubdivide();
    REQUIRE(fine.validate().empty());
    REQUIRE(fine.faceCount() == 4 + 3);  // quad -> 4 quads, tri -> 3 quads
    const auto* fineColors = fine.vertexAttributes().find<Vec3>("color");
    REQUIRE(fineColors != nullptr);
    for (Index i = 0; i < fine.vertexCapacity(); ++i) {
        if (fine.isAlive(VertexId{i})) {
            REQUIRE((*fineColors)[i].x == doctest::Approx(1.0f));
            REQUIRE((*fineColors)[i].y == doctest::Approx(0.0f));
        }
    }
}

TEST_CASE("subdivision preserves feature flags on child edges") {
    Mesh cube = makeCube();
    cube.tagFeatureEdges(90.0f);
    Mesh fine = cube.linearSubdivide();
    REQUIRE(fine.validate().empty());
    std::size_t features = 0;
    for (Index i = 0; i < fine.edgeCapacity(); ++i) {
        if (fine.isAlive(cyber::EdgeId{i}) && fine.isFeatureEdge(cyber::EdgeId{i})) {
            ++features;
        }
    }
    REQUIRE(features == 24);  // each of the 12 cube edges split in two
}
