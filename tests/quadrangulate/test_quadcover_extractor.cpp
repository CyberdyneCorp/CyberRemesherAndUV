#include <doctest.h>

#include <vector>

#include "cyber/core/mesh.hpp"
#include "cyber/quadrangulate/quadcover_extractor.hpp"

using cyber::FaceId;
using cyber::Index;
using cyber::Mesh;
using cyber::Vec3;
namespace remesh = cyber::remesh;

namespace {

// Two triangles forming a unit quad in the z = 0 plane.
Mesh makeTwoTri() {
    std::vector<Vec3> p = {{0, 0, 0}, {1, 0, 0}, {1, 1, 0}, {0, 1, 0}};
    std::vector<std::vector<Index>> f = {{0, 1, 2}, {0, 2, 3}};
    return Mesh::fromIndexed(p, f);
}

std::size_t aliveFaces(const Mesh& mesh) {
    std::size_t n = 0;
    for (Index i = 0; i < mesh.faceCapacity(); ++i) {
        if (mesh.isAlive(FaceId{i})) {
            ++n;
        }
    }
    return n;
}

}  // namespace

// TASK F scaffold contract. Until the seamless-UV isoline extractor is
// implemented, the maker must still yield a usable IQuadrangulator that fails
// cleanly and never corrupts the input mesh. Locking this now keeps the pipeline
// safe when the quad-cover method is selected before it is finished, and gives
// the next engineer a red/green target to flip once extraction lands.
TEST_CASE("quad-cover quadrangulator is a safe not-implemented stub") {
    auto q = remesh::makeQuadCoverQuadrangulator();
    REQUIRE(q != nullptr);
    CHECK(q->name() == "quad-cover");

    Mesh mesh = makeTwoTri();
    const std::size_t facesBefore = aliveFaces(mesh);

    auto outcome = q->quadrangulate(mesh, 1.0f, nullptr, nullptr);

    // Not implemented yet: reports failure, not cancellation, with a reason.
    CHECK_FALSE(outcome.success);
    CHECK_FALSE(outcome.cancelled);
    CHECK_FALSE(outcome.failureReason.empty());

    // The mesh must be left exactly as-is on the failure path.
    CHECK(aliveFaces(mesh) == facesBefore);
}

// The collaborator seams compile and return their well-defined empty results.
TEST_CASE("quad-cover collaborator stubs return empty/invalid results") {
    Mesh mesh = makeTwoTri();
    auto uv = remesh::computeSeamlessUv(mesh, 1.0f);
    CHECK_FALSE(uv.valid);
    CHECK(uv.triangleUv.empty());

    auto out = remesh::extractIsolineQuads(mesh, uv);
    CHECK(out.vertices.empty());
    CHECK(out.quads.empty());
}
