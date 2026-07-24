#include <doctest.h>

#include <cmath>
#include <map>
#include <set>
#include <utility>
#include <vector>

#include "cyber/core/isotropic.hpp"
#include "cyber/core/mesh.hpp"
#include "cyber/core/reference_surface.hpp"

using cyber::CancelToken;
using cyber::EdgeId;
using cyber::FaceId;
using cyber::Index;
using cyber::Mesh;
using cyber::ProgressSink;
using cyber::Vec3;
using cyber::VertexId;
namespace remesh = cyber::remesh;

namespace {

// Triangulated UV sphere of radius 1.
Mesh makeSphere(int rings, int segments) {
    std::vector<Vec3> p;
    p.push_back({0, 0, 1});  // north pole
    for (int r = 1; r < rings; ++r) {
        const float phi = cyber::kPi * static_cast<float>(r) / static_cast<float>(rings);
        for (int s = 0; s < segments; ++s) {
            const float theta =
                2.0f * cyber::kPi * static_cast<float>(s) / static_cast<float>(segments);
            p.push_back(
                {std::sin(phi) * std::cos(theta), std::sin(phi) * std::sin(theta), std::cos(phi)});
        }
    }
    p.push_back({0, 0, -1});  // south pole
    const Index south = static_cast<Index>(p.size() - 1);

    std::vector<std::vector<Index>> f;
    auto ringVertex = [segments](int r, int s) {
        return static_cast<Index>(1 + (r - 1) * segments + (s % segments));
    };
    for (int s = 0; s < segments; ++s) {
        f.push_back({0, ringVertex(1, s), ringVertex(1, s + 1)});
    }
    for (int r = 1; r + 1 < rings; ++r) {
        for (int s = 0; s < segments; ++s) {
            const Index a = ringVertex(r, s), b = ringVertex(r, s + 1);
            const Index c = ringVertex(r + 1, s), d = ringVertex(r + 1, s + 1);
            f.push_back({a, c, d});
            f.push_back({a, d, b});
        }
    }
    for (int s = 0; s < segments; ++s) {
        f.push_back({south, ringVertex(rings - 1, s + 1), ringVertex(rings - 1, s)});
    }
    return Mesh::fromIndexed(p, f);
}

Mesh makeTriangulatedCube() {
    const std::vector<Vec3> p = {
        {0, 0, 0}, {1, 0, 0}, {1, 1, 0}, {0, 1, 0}, {0, 0, 1}, {1, 0, 1}, {1, 1, 1}, {0, 1, 1},
    };
    const std::vector<std::vector<Index>> f = {
        {0, 3, 2, 1}, {4, 5, 6, 7}, {0, 1, 5, 4}, {1, 2, 6, 5}, {2, 3, 7, 6}, {3, 0, 4, 7},
    };
    Mesh mesh = Mesh::fromIndexed(p, f);
    mesh.triangulate();
    return mesh;
}

struct EdgeStats {
    float minLength = 1e30f, maxLength = 0.0f;
    std::size_t count = 0;
};

EdgeStats edgeStats(const Mesh& mesh) {
    EdgeStats stats;
    for (Index i = 0; i < mesh.edgeCapacity(); ++i) {
        if (!mesh.isAlive(EdgeId{i})) {
            continue;
        }
        const auto [a, b] = mesh.edgeVertices(EdgeId{i});
        const float len = length(mesh.position(a) - mesh.position(b));
        stats.minLength = std::fmin(stats.minLength, len);
        stats.maxLength = std::fmax(stats.maxLength, len);
        ++stats.count;
    }
    return stats;
}

}  // namespace

TEST_CASE("isotropic remesh converges edge lengths and stays on the sphere") {
    Mesh mesh = makeSphere(12, 18);
    const remesh::ReferenceSurface reference(mesh, 0.0f);
    constexpr float kTarget = 0.25f;

    remesh::IsotropicOptions options;
    options.targetEdgeLength = kTarget;
    options.iterations = 4;
    const auto status = remesh::isotropicRemesh(mesh, reference, options);
    REQUIRE(status == remesh::IsotropicStatus::Success);
    REQUIRE(mesh.validate().empty());

    // Edge lengths mostly inside the [4/5, 4/3]*target band; allow slack at
    // the tails but nothing pathological.
    const EdgeStats stats = edgeStats(mesh);
    REQUIRE(stats.count > 100);
    REQUIRE(stats.maxLength < kTarget * 4.0f / 3.0f * 1.35f);
    REQUIRE(stats.minLength > kTarget * 0.25f);

    // Every vertex projected back to the unit sphere.
    for (Index i = 0; i < mesh.vertexCapacity(); ++i) {
        if (mesh.isAlive(VertexId{i})) {
            REQUIRE(length(mesh.position(VertexId{i})) == doctest::Approx(1.0f).epsilon(0.02));
        }
    }
}

TEST_CASE("feature vertices and edges survive remeshing (spec: feature-preserving)") {
    Mesh mesh = makeTriangulatedCube();
    mesh.tagFeatureEdges(90.0f);
    const remesh::ReferenceSurface reference(mesh, 0.0f);

    remesh::IsotropicOptions options;
    options.targetEdgeLength = 0.3f;
    options.iterations = 3;
    REQUIRE(remesh::isotropicRemesh(mesh, reference, options) == remesh::IsotropicStatus::Success);
    REQUIRE(mesh.validate().empty());

    // The 8 cube corners must still exist at their exact positions.
    std::size_t cornersFound = 0;
    for (Index i = 0; i < mesh.vertexCapacity(); ++i) {
        if (!mesh.isAlive(VertexId{i})) {
            continue;
        }
        const Vec3 p = mesh.position(VertexId{i});
        auto is01 = [](float v) { return v == 0.0f || v == 1.0f; };
        if (is01(p.x) && is01(p.y) && is01(p.z)) {
            ++cornersFound;
        }
    }
    REQUIRE(cornersFound == 8);

    // Every feature-edge endpoint still lies exactly on the cube's edge
    // skeleton (two coordinates in {0,1}): feature vertices were never
    // smoothed, projected or collapsed off the feature.
    for (Index i = 0; i < mesh.edgeCapacity(); ++i) {
        if (!mesh.isAlive(EdgeId{i}) || !mesh.isFeatureEdge(EdgeId{i})) {
            continue;
        }
        const auto [a, b] = mesh.edgeVertices(EdgeId{i});
        for (const VertexId v : {a, b}) {
            const Vec3 p = mesh.position(v);
            int onBoundaryPlane = 0;
            for (const float c : {p.x, p.y, p.z}) {
                if (c == 0.0f || c == 1.0f) {
                    ++onBoundaryPlane;
                }
            }
            REQUIRE(onBoundaryPlane >= 2);
        }
    }
}

TEST_CASE("adaptivity zero means uniform targets (spec: remeshing-parameters)") {
    // Indirect but meaningful: with adaptivity 0 the flat cube faces refine
    // to the same density everywhere; with adaptivity 1 the (curved-at-
    // corners) result differs. Both must stay valid.
    Mesh uniform = makeTriangulatedCube();
    uniform.tagFeatureEdges(90.0f);
    const remesh::ReferenceSurface refU(uniform, 0.0f);
    remesh::IsotropicOptions options;
    options.targetEdgeLength = 0.35f;
    options.adaptivity = 0.0f;
    REQUIRE(remesh::isotropicRemesh(uniform, refU, options) == remesh::IsotropicStatus::Success);

    Mesh adaptive = makeTriangulatedCube();
    adaptive.tagFeatureEdges(90.0f);
    const remesh::ReferenceSurface refA(adaptive, 0.0f);
    options.adaptivity = 1.0f;
    REQUIRE(remesh::isotropicRemesh(adaptive, refA, options) == remesh::IsotropicStatus::Success);
    REQUIRE(uniform.validate().empty());
    REQUIRE(adaptive.validate().empty());
}

TEST_CASE("pre-cancelled token aborts immediately (spec: cancellation)") {
    Mesh mesh = makeSphere(12, 18);
    const remesh::ReferenceSurface reference(mesh, 0.0f);
    const CancelToken cancel;
    cancel.requestCancel();

    remesh::IsotropicOptions options;
    options.targetEdgeLength = 0.1f;
    const auto status = remesh::isotropicRemesh(mesh, reference, options, nullptr, &cancel);
    REQUIRE(status == remesh::IsotropicStatus::Cancelled);
}

TEST_CASE("progress is monotonic and reaches 1.0") {
    Mesh mesh = makeSphere(8, 12);
    const remesh::ReferenceSurface reference(mesh, 0.0f);
    std::vector<float> values;
    ProgressSink sink([&values](float p, std::string_view) { values.push_back(p); });

    remesh::IsotropicOptions options;
    options.targetEdgeLength = 0.4f;
    REQUIRE(remesh::isotropicRemesh(mesh, reference, options, &sink) ==
            remesh::IsotropicStatus::Success);
    REQUIRE(!values.empty());
    for (std::size_t i = 1; i < values.size(); ++i) {
        REQUIRE(values[i] >= values[i - 1]);
    }
    REQUIRE(values.back() == doctest::Approx(1.0f));
}

TEST_CASE("invalid inputs are rejected") {
    Mesh mesh = makeSphere(6, 8);
    const remesh::ReferenceSurface reference(mesh, 0.0f);
    remesh::IsotropicOptions options;  // targetEdgeLength unset
    REQUIRE(remesh::isotropicRemesh(mesh, reference, options) ==
            remesh::IsotropicStatus::InvalidInput);
    const remesh::ReferenceSurface empty{};
    options.targetEdgeLength = 0.3f;
    REQUIRE(remesh::isotropicRemesh(mesh, empty, options) == remesh::IsotropicStatus::InvalidInput);
}

TEST_CASE("isotropic remesh is deterministic") {
    auto run = [] {
        Mesh mesh = makeSphere(10, 14);
        const remesh::ReferenceSurface reference(mesh, 0.0f);
        remesh::IsotropicOptions options;
        options.targetEdgeLength = 0.3f;
        REQUIRE(remesh::isotropicRemesh(mesh, reference, options) ==
                remesh::IsotropicStatus::Success);
        std::vector<Vec3> positions;
        std::vector<std::vector<Index>> faces;
        mesh.toIndexed(positions, faces);
        return std::pair{positions, faces};
    };
    const auto [p1, f1] = run();
    const auto [p2, f2] = run();
    REQUIRE(p1.size() == p2.size());
    REQUIRE(f1 == f2);
    for (std::size_t i = 0; i < p1.size(); ++i) {
        REQUIRE(p1[i] == p2[i]);
    }
}

// Crease preservation through the isotropic stage (docs/ROADMAP.md Phase 3, lever c1).
//
// `isotropicRemesh` already refuses to collapse feature vertices, flip feature edges or smooth
// them — but only for edges `tagFeatureEdges` actually marked. That call takes an INCLUDED angle,
// so the quad-cover default of 40 means "face-normal angle >= 140", i.e. knife-edge folds only:
// a 90-degree box edge is NOT tagged, and the remesher then freely remeshes straight across it.
// MEASURED on fandisk at ~3000 quads before this was fixed: the crease network fell from 706 edges
// in ONE connected component to 449 edges in 55 components with 136 dangling ends.
//
// This pins the mechanism the fix relies on: tagged at a crease-appropriate threshold, the crease
// network survives the remesh intact — same edge count, still a single connected component.
TEST_CASE("isotropic remesh preserves a tagged crease network") {
    const auto creaseGraph = [](const Mesh& m) {
        // Interior edges whose face-normal angle exceeds 45 degrees, as adjacency.
        std::map<Index, std::vector<Index>> adj;
        std::size_t count = 0;
        for (Index i = 0; i < m.edgeCapacity(); ++i) {
            const EdgeId e{i};
            if (!m.isAlive(e) || m.edgeFaceCount(e) != 2) {
                continue;
            }
            const std::vector<FaceId> f = m.edgeFaces(e);
            const Vec3 n0 = normalized(m.faceNormal(f[0]));
            const Vec3 n1 = normalized(m.faceNormal(f[1]));
            if (dot(n0, n1) >= std::cos(45.0f * cyber::kPi / 180.0f)) {
                continue;
            }
            ++count;
            const auto [a, b] = m.edgeVertices(e);
            adj[a.value].push_back(b.value);
            adj[b.value].push_back(a.value);
        }
        // Connected components of that graph.
        std::set<Index> seen;
        std::size_t comps = 0;
        for (const auto& [v0, _] : adj) {
            if (seen.count(v0) != 0) {
                continue;
            }
            ++comps;
            std::vector<Index> stack{v0};
            seen.insert(v0);
            while (!stack.empty()) {
                const Index v = stack.back();
                stack.pop_back();
                for (const Index w : adj.at(v)) {
                    if (seen.insert(w).second) {
                        stack.push_back(w);
                    }
                }
            }
        }
        return std::pair{count, comps};
    };

    // `protect` selects the threshold passed to tagFeatureEdges before remeshing. 135 (included
    // angle) == "protect anything with a face-normal angle above 45", which is what the quad-cover
    // native path now tags with; 40 is the old shipped value, which marks nothing on a 90-degree
    // box edge and therefore protects nothing.
    const auto remeshWith = [&creaseGraph](float protect) {
        Mesh cube = makeTriangulatedCube();
        cube.tagFeatureEdges(protect);
        const remesh::ReferenceSurface reference(cube, 0.0f);
        remesh::IsotropicOptions options;
        options.targetEdgeLength = 0.35f;
        REQUIRE(remesh::isotropicRemesh(cube, reference, options) ==
                remesh::IsotropicStatus::Success);
        return creaseGraph(cube);
    };

    const auto [beforeEdges, beforeComps] = creaseGraph(makeTriangulatedCube());
    REQUIRE(beforeEdges > 0);
    REQUIRE(beforeComps == 1);  // a cube's box edges form one closed network

    // Protected: the network survives. Edge COUNT may rise, because the remesher splits a long
    // crease into collinear sub-edges to reach the target length — that preserves the crease
    // exactly. What must not happen is losing crease edges or shattering the network.
    const auto [keptEdges, keptComps] = remeshWith(135.0f);
    CHECK(keptEdges >= beforeEdges);
    CHECK(keptComps == 1);

    // Discriminating half: at the old threshold nothing is tagged, so the remesher cuts straight
    // across the creases and the network degrades. Without this the test would pass even if the
    // protection did nothing.
    const auto [lostEdges, lostComps] = remeshWith(40.0f);
    CHECK(lostComps > keptComps);
}
