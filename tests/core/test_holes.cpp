#include <doctest.h>

#include <vector>

#include "cyber/core/mesh.hpp"

using cyber::EdgeId;
using cyber::FaceId;
using cyber::Index;
using cyber::Mesh;
using cyber::Vec3;

namespace {

// n x n grid of quads ((n+1)^2 vertices, n*n faces) in the z = 0 plane.
// Face (i, j) has index i*n + j; its outer boundary has 4*n edges.
Mesh makeQuadGrid(int n) {
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
            f.push_back({idx(i, j), idx(i + 1, j), idx(i + 1, j + 1), idx(i, j + 1)});
        }
    }
    return Mesh::fromIndexed(p, f);
}

std::size_t boundaryEdgeCount(const Mesh& mesh) {
    std::size_t count = 0;
    for (Index i = 0; i < mesh.edgeCapacity(); ++i) {
        if (mesh.isAlive(EdgeId{i}) && mesh.isBoundaryEdge(EdgeId{i})) {
            ++count;
        }
    }
    return count;
}

}  // namespace

TEST_CASE("fillHoles closes a small interior hole within the limit") {
    Mesh grid = makeQuadGrid(4);
    // Face index 5 = interior quad (i=1, j=1); removing it opens a 4-edge hole
    // while the 16-edge outer boundary stays open.
    grid.removeFace(FaceId{5});
    REQUIRE(grid.faceCount() == 15);
    REQUIRE(boundaryEdgeCount(grid) == 20);  // 16 outer + 4 hole

    const std::size_t filled = grid.fillHoles(4);
    REQUIRE(filled == 1);
    REQUIRE(grid.validate().empty());
    REQUIRE(grid.faceCount() == 16);
    REQUIRE(boundaryEdgeCount(grid) == 16);  // only the outer boundary remains
}

TEST_CASE("fillHoles leaves holes larger than the limit untouched") {
    Mesh grid = makeQuadGrid(4);
    grid.removeFace(FaceId{5});

    // Limit below the 4-edge hole: nothing is filled; the outer boundary is
    // never a fill target here either.
    REQUIRE(grid.fillHoles(3) == 0);
    REQUIRE(grid.faceCount() == 15);
    REQUIRE(boundaryEdgeCount(grid) == 20);
}

TEST_CASE("fillHoles with a disabling limit is a no-op") {
    Mesh grid = makeQuadGrid(4);
    grid.removeFace(FaceId{5});
    REQUIRE(grid.fillHoles(0) == 0);
    REQUIRE(grid.fillHoles(2) == 0);
    REQUIRE(grid.faceCount() == 15);
}

TEST_CASE("fillHoles keeps orientation consistent across a 6-edge hole") {
    // Remove two adjacent interior faces (i=1,j=1) and (i=1,j=2) -> a single
    // 6-edge interior hole; filling it with the limit at 6 must produce a
    // manifold, validate-clean mesh.
    Mesh grid = makeQuadGrid(4);
    grid.removeFace(FaceId{5});  // (1,1)
    grid.removeFace(FaceId{6});  // (1,2), shares an edge with (1,1)
    REQUIRE(boundaryEdgeCount(grid) == 22);  // 16 outer + 6 hole

    const std::size_t filled = grid.fillHoles(6);
    REQUIRE(filled == 1);
    REQUIRE(grid.validate().empty());
    REQUIRE(grid.faceCount() == 15);
    REQUIRE(boundaryEdgeCount(grid) == 16);
}
