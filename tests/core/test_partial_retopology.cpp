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
    Mesh source = makePatch();
    auto& weights = source.vertexAttributes().create<float>("weight");
    auto& groups = source.vertexAttributes().create<std::int32_t>("group_id");
    auto& materials = source.faceAttributes().create<std::int32_t>("material_id");
    materials[5] = 42;
    source.cornerAttributes().create<cyber::Vec2>("uv");
    for (Index vertex = 0; vertex < source.vertexCapacity(); ++vertex) {
        weights[vertex] = static_cast<float>(vertex);
        groups[vertex] = static_cast<std::int32_t>(vertex);
    }
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
    REQUIRE(result.correspondences.size() == 4);
    REQUIRE(result.sourceVertexToOutput.size() == source.vertexCapacity());
    CHECK(result.sourceVertexToOutput[6] == cyber::VertexId{6});
    REQUIRE(result.untransferredAttributes.size() == 1);
    CHECK(result.untransferredAttributes[0] == "corner:uv");
    const auto* transferredWeights = result.mesh.vertexAttributes().find<float>("weight");
    const auto* transferredGroups = result.mesh.vertexAttributes().find<std::int32_t>("group_id");
    REQUIRE(transferredWeights != nullptr);
    REQUIRE(transferredGroups != nullptr);
    const auto* transferredMaterials =
        result.mesh.faceAttributes().find<std::int32_t>("material_id");
    REQUIRE(transferredMaterials != nullptr);
    for (const remesh::SourceCorrespondence& correspondence : result.correspondences) {
        CHECK(correspondence.sourceFace == FaceId{5});
        CHECK(correspondence.distance == doctest::Approx(0.0f));
        CHECK(correspondence.confidence == doctest::Approx(1.0f));
        CHECK(correspondence.barycentric.x + correspondence.barycentric.y +
                  correspondence.barycentric.z ==
              doctest::Approx(1.0f));
        CHECK((*transferredWeights)[correspondence.outputVertex.value] ==
              doctest::Approx(
                  correspondence.barycentric.x * weights[correspondence.sourceTriangle[0].value] +
                  correspondence.barycentric.y * weights[correspondence.sourceTriangle[1].value] +
                  correspondence.barycentric.z * weights[correspondence.sourceTriangle[2].value]));
        const std::int32_t transferred = (*transferredGroups)[correspondence.outputVertex.value];
        const bool transferredFromTriangle =
            transferred == groups[correspondence.sourceTriangle[0].value] ||
            transferred == groups[correspondence.sourceTriangle[1].value] ||
            transferred == groups[correspondence.sourceTriangle[2].value];
        CHECK(transferredFromTriangle);
    }
    for (std::size_t face = 0; face < result.mesh.faceCapacity(); ++face) {
        if (result.mesh.isAlive(FaceId{static_cast<Index>(face)})) {
            CHECK(result.mesh.faceSize(FaceId{static_cast<Index>(face)}) == 4);
            if (face >= 16 || face == 5) {
                CHECK((*transferredMaterials)[face] == 42);
            }
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
