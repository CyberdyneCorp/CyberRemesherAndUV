#include <doctest.h>

#include <vector>

#include "cyber/core/mesh.hpp"
#include "cyber/core/partial_retopology.hpp"

using cyber::FaceId;
using cyber::Index;
using cyber::Mesh;
using cyber::Vec3;
using cyber::VertexId;
namespace remesh = cyber::remesh;

namespace {

Mesh makePatch() {
    std::vector<Vec3> points;
    std::vector<std::vector<Index>> faces;
    for (Index y = 0; y != 5; ++y) {
        for (Index x = 0; x != 5; ++x) {
            points.push_back({static_cast<float>(x), static_cast<float>(y), 0.0f});
        }
    }
    for (Index y = 0; y != 4; ++y) {
        for (Index x = 0; x != 4; ++x) {
            const Index base = y * 5 + x;
            faces.push_back({base, base + 1, base + 6, base + 5});
        }
    }
    return Mesh::fromIndexed(points, faces);
}

}  // namespace

TEST_CASE("partial retopology accepts one manifold four-edge region") {
    const Mesh mesh = makePatch();
    const remesh::PartialRetopologyAnalysis analysis =
        remesh::analyzePartialRetopology(mesh, {{FaceId{5}}, true});
    REQUIRE(analysis.status == remesh::PartialRetopologyStatus::Ready);
    REQUIRE(analysis.boundary.size() == 4);
    CHECK(analysis.boundary[0].value == 6);
}

TEST_CASE("partial retopology rejects disconnected or parity-incompatible regions") {
    const Mesh mesh = makePatch();
    const auto disconnected =
        remesh::analyzePartialRetopology(mesh, {{FaceId{5}, FaceId{10}}, true});
    CHECK(disconnected.status == remesh::PartialRetopologyStatus::Rejected);
    CHECK(disconnected.reason == "selected faces are disconnected");

    const auto larger = remesh::analyzePartialRetopology(mesh, {{FaceId{5}, FaceId{6}}, true});
    CHECK(larger.status == remesh::PartialRetopologyStatus::Rejected);
    CHECK(larger.reason.find("four-edge boundary") != std::string::npos);
}

TEST_CASE("partial retopology replaces only the selected region") {
    const Mesh source = makePatch();
    const std::vector<VertexId> exteriorFace = source.faceVertices(FaceId{0});
    const Vec3 borderPosition = source.position({6});

    const remesh::PartialRetopologyResult result =
        remesh::partialRetopologize(source, {{FaceId{5}}, true});

    REQUIRE(result.status == remesh::PartialRetopologyStatus::Applied);
    CHECK(source.faceCount() == 16);
    CHECK(result.mesh.faceCount() == 20);
    const std::vector<VertexId> resultExteriorFace = result.mesh.faceVertices(FaceId{0});
    REQUIRE(resultExteriorFace.size() == exteriorFace.size());
    for (std::size_t i = 0; i < exteriorFace.size(); ++i) {
        CHECK(resultExteriorFace[i] == exteriorFace[i]);
    }
    CHECK(result.mesh.position({6}) == borderPosition);
    CHECK(result.mesh.validate().empty());
    for (std::size_t face = 0; face < result.mesh.faceCapacity(); ++face) {
        if (result.mesh.isAlive(FaceId{static_cast<Index>(face)})) {
            CHECK(result.mesh.faceSize(FaceId{static_cast<Index>(face)}) == 4);
        }
    }
}

TEST_CASE("rejected partial retopology returns an unchanged mesh") {
    const Mesh source = makePatch();
    const remesh::PartialRetopologyResult result =
        remesh::partialRetopologize(source, {{FaceId{5}, FaceId{6}}, true});

    CHECK(result.status == remesh::PartialRetopologyStatus::Rejected);
    CHECK(result.mesh.faceCount() == source.faceCount());
    CHECK(result.mesh.validate().empty());
}
