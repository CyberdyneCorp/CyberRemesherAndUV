#include <doctest.h>

#include <cmath>
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

// A closed UV sphere — a well-formed input for the seamless-UV solver.
Mesh makeSphere(int rings = 16, int segments = 24) {
    std::vector<Vec3> p;
    p.push_back({0, 0, 1});
    for (int r = 1; r < rings; ++r) {
        const float phi = 3.14159265f * static_cast<float>(r) / static_cast<float>(rings);
        for (int s = 0; s < segments; ++s) {
            const float th = 2.0f * 3.14159265f * static_cast<float>(s) / static_cast<float>(segments);
            p.push_back({std::sin(phi) * std::cos(th), std::sin(phi) * std::sin(th), std::cos(phi)});
        }
    }
    p.push_back({0, 0, -1});
    const Index south = static_cast<Index>(p.size() - 1);
    const auto ring = [&](int r, int s) {
        return static_cast<Index>(1 + (r - 1) * segments + (s % segments));
    };
    std::vector<std::vector<Index>> f;
    for (int s = 0; s < segments; ++s) {
        f.push_back({0, ring(1, s), ring(1, s + 1)});
    }
    for (int r = 1; r + 1 < rings; ++r) {
        for (int s = 0; s < segments; ++s) {
            f.push_back({ring(r, s), ring(r + 1, s), ring(r + 1, s + 1)});
            f.push_back({ring(r, s), ring(r + 1, s + 1), ring(r, s + 1)});
        }
    }
    for (int s = 0; s < segments; ++s) {
        f.push_back({south, ring(rings - 1, s + 1), ring(rings - 1, s)});
    }
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

// The isoline tracer (Milestone 2) is still a stub: an invalid UV yields no quads.
TEST_CASE("quad-cover isoline extractor returns empty for an invalid UV") {
    Mesh mesh = makeTwoTri();
    const remesh::SeamlessUv invalid;  // valid == false
    auto out = remesh::extractIsolineQuads(mesh, invalid);
    CHECK(out.vertices.empty());
    CHECK(out.quads.empty());
}

// Milestone 1: computeSeamlessUv obtains a seamless integer-grid UV out-of-process
// from AutoRemesher's Geogram quad_cover when CYBER_QUADCOVER_CLI points at a built
// autoremesher_cli. The UV must be genuinely seamless — the integer-jump residual
// across every interior edge is ~0. Without the harness (env unset / build absent) it
// degrades cleanly to an invalid UV, so the test is a no-op there.
TEST_CASE("quad-cover M1: harness seamless UV has zero integer-jump residual") {
    const Mesh sphere = makeSphere();
    const remesh::SeamlessUv uv = remesh::computeSeamlessUv(sphere, 0.15f);
    if (!uv.valid) {
        CHECK(uv.triangles.empty());  // harness unavailable -> clean degrade
        return;
    }
    CHECK(uv.triangles.size() == uv.triangleUv.size());
    CHECK(uv.vertices.size() > 0);
    CHECK(remesh::seamlessUvResidual(uv) < 1e-3);  // seamless by construction
}
