#include <doctest.h>

#include <array>
#include <cmath>
#include <cstddef>
#include <vector>

#include "cyber/core/mesh.hpp"
#include "cyber/quadrangulate/quadcover_extractor.hpp"

using cyber::EdgeId;
using cyber::FaceId;
using cyber::Index;
using cyber::Mesh;
using cyber::Vec2;
using cyber::Vec3;
using cyber::VertexId;
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

// Build a SeamlessUv directly for a flat N x N grid of quad cells in the z = 0
// plane. Vertex (i, j) sits at world (i, j, 0) and carries UV = (i, j), so the
// integer isolines u,v in 0..N land exactly on the grid lines. Each cell is split
// into two triangles. Feeding this to extractIsolineQuads must recover the N x N
// quad grid: the isoline tracer's crossings are the grid vertices, valence 4 by
// construction on the interior.
remesh::SeamlessUv makeFlatGridUv(int n) {
    remesh::SeamlessUv uv;
    const auto vid = [&](int i, int j) { return static_cast<Index>(j * (n + 1) + i); };
    for (int j = 0; j <= n; ++j) {
        for (int i = 0; i <= n; ++i) {
            uv.vertices.push_back(Vec3{static_cast<float>(i), static_cast<float>(j), 0.0f});
        }
    }
    const auto uvOf = [](Index v, int n1) {
        const int i = static_cast<int>(v) % (n1 + 1);
        const int j = static_cast<int>(v) / (n1 + 1);
        return Vec2{static_cast<float>(i), static_cast<float>(j)};
    };
    const auto addTri = [&](Index a, Index b, Index c) {
        uv.triangles.push_back({a, b, c});
        uv.triangleUv.push_back({uvOf(a, n), uvOf(b, n), uvOf(c, n)});
    };
    for (int j = 0; j < n; ++j) {
        for (int i = 0; i < n; ++i) {
            const Index v00 = vid(i, j);
            const Index v10 = vid(i + 1, j);
            const Index v11 = vid(i + 1, j + 1);
            const Index v01 = vid(i, j + 1);
            addTri(v00, v10, v11);
            addTri(v00, v11, v01);
        }
    }
    uv.valid = true;
    return uv;
}

// Valence (incident alive faces) of a vertex, and whether it touches a boundary
// edge (an edge with a single incident face).
struct VertexInfo {
    std::size_t faces = 0;
    bool boundary = false;
};
VertexInfo vertexInfo(const Mesh& mesh, VertexId v) {
    VertexInfo info;
    info.faces = mesh.vertexFaces(v).size();
    for (const EdgeId e : mesh.vertexEdges(v)) {
        if (mesh.isBoundaryEdge(e)) {
            info.boundary = true;
        }
    }
    return info;
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

// Milestone 2 PRIMARY checkpoint: the isoline tracer on a flat integer-grid UV must
// recover a clean N x N quad grid. This exercises the tracer core deterministically
// (no external harness): extractConnections -> extractEdges -> extractMesh must find
// the grid cells as transversal-crossing quads. The result is an open disk, so its
// perimeter is a legitimate boundary; every INTERIOR vertex must be valence 4 (zero
// irregular), all faces must be quads, and the half-edge structure must be sound.
TEST_CASE("quad-cover M2: flat integer-grid UV extracts a clean quad grid") {
    const int n = 6;
    const remesh::SeamlessUv uv = makeFlatGridUv(n);
    Mesh dummy = makeTwoTri();  // mesh param is ignored by the extractor

    const remesh::IsolineQuadMesh out = remesh::extractIsolineQuads(dummy, uv);

    REQUIRE_FALSE(out.quads.empty());
    for (const auto& face : out.quads) {
        CHECK(face.size() == 4);
    }

    // Rebuild as a cyber::Mesh to check topology.
    std::vector<std::vector<Index>> faces;
    faces.reserve(out.quads.size());
    for (const auto& q : out.quads) {
        std::vector<Index> f;
        f.reserve(q.size());
        for (const std::size_t v : q) {
            f.push_back(static_cast<Index>(v));
        }
        faces.push_back(std::move(f));
    }
    const Mesh mesh = Mesh::fromIndexed(out.vertices, faces);

    // Structural half-edge invariants must all hold (fromIndexed is non-manifold
    // safe, so this catches any face that references a stale/duplicate vertex).
    CHECK(mesh.validate().empty());

    // Interior vertices (not touching a boundary edge) must be regular (valence 4);
    // zero interior irregular vertices is the whole point of transversal crossings.
    std::size_t interior = 0;
    std::size_t interiorIrregular = 0;
    for (Index i = 0; i < mesh.vertexCapacity(); ++i) {
        const VertexId v{i};
        if (!mesh.isAlive(v)) {
            continue;
        }
        const VertexInfo info = vertexInfo(mesh, v);
        if (info.boundary) {
            continue;
        }
        ++interior;
        if (info.faces != 4) {
            ++interiorIrregular;
        }
    }
    CHECK(interior > 0);
    CHECK(interiorIrregular == 0);

    // A clean 6x6 grid has 36 quads and 25 interior vertices; allow no irregular
    // interior but don't over-fit the exact face count (boundary handling may add a
    // few perimeter faces). The grid must at least cover its interior.
    CHECK(interior == static_cast<std::size_t>((n - 1) * (n - 1)));
}

// Milestone 2 SECONDARY (needs the M1 harness): a real seamless UV on a sphere runs
// through the isoline tracer end to end. No-op when the harness is unavailable.
//
// NOTE: on a dense/curved surface the raw isoline graph carries many valence-2
// mid-isoline sample points; merging them into clean quad cells is the closed-surface
// graph-cleanup stage, which is ported but disabled by default (see extract() /
// TODO(M3)). So this test only asserts the tracer runs and produces a constructible
// mesh — it does NOT yet assert a fully clean closed quad mesh (that is M3).
TEST_CASE("quad-cover M2: harness sphere UV runs the isoline tracer") {
    const Mesh sphere = makeSphere();
    const remesh::SeamlessUv uv = remesh::computeSeamlessUv(sphere, 0.15f);
    if (!uv.valid) {
        CHECK(uv.triangles.empty());  // harness unavailable -> clean degrade
        return;
    }
    const remesh::IsolineQuadMesh out = remesh::extractIsolineQuads(sphere, uv);
    REQUIRE_FALSE(out.quads.empty());
    CHECK(out.vertices.size() > 0);
    for (const auto& face : out.quads) {
        CHECK(face.size() >= 3);  // every polygon is at least a triangle
    }

    // The result must at least be constructible as a mesh (fromIndexed is
    // non-manifold safe; this catches faces referencing out-of-range vertices).
    std::vector<std::vector<Index>> faces;
    faces.reserve(out.quads.size());
    for (const auto& q : out.quads) {
        std::vector<Index> f;
        for (const std::size_t v : q) {
            f.push_back(static_cast<Index>(v));
        }
        faces.push_back(std::move(f));
    }
    const Mesh mesh = Mesh::fromIndexed(out.vertices, faces);
    CHECK(mesh.faceCount() > 0);
}
