// Property tests (mesh-core spec, "Structural invariants after every
// operator" and "Deterministic operations"): random operator sequences with
// the invariant validator run after every single mutation.
#include <doctest.h>

#include <cstdint>
#include <random>
#include <vector>

#include "cyber/core/mesh.hpp"

using cyber::EdgeId;
using cyber::FaceId;
using cyber::Index;
using cyber::Mesh;
using cyber::Vec3;
using cyber::VertexId;

namespace {

Mesh makeGrid(int nx, int ny) {
    std::vector<Vec3> p;
    for (int y = 0; y <= ny; ++y) {
        for (int x = 0; x <= nx; ++x) {
            p.push_back({static_cast<float>(x), static_cast<float>(y),
                         0.1f * static_cast<float>((x * 7 + y * 3) % 5)});
        }
    }
    std::vector<std::vector<Index>> f;
    for (Index y = 0; y < static_cast<Index>(ny); ++y) {
        for (Index x = 0; x < static_cast<Index>(nx); ++x) {
            const Index i = y * static_cast<Index>(nx + 1) + x;
            f.push_back({i, i + 1, i + static_cast<Index>(nx + 2), i + static_cast<Index>(nx + 1)});
        }
    }
    return Mesh::fromIndexed(p, f);
}

std::vector<EdgeId> aliveEdges(const Mesh& mesh) {
    std::vector<EdgeId> edges;
    for (Index i = 0; i < mesh.edgeCapacity(); ++i) {
        if (mesh.isAlive(EdgeId{i})) {
            edges.push_back(EdgeId{i});
        }
    }
    return edges;
}

std::vector<FaceId> aliveFaces(const Mesh& mesh) {
    std::vector<FaceId> faces;
    for (Index i = 0; i < mesh.faceCapacity(); ++i) {
        if (mesh.isAlive(FaceId{i})) {
            faces.push_back(FaceId{i});
        }
    }
    return faces;
}

// One deterministic random walk over the operator set.
void runOperatorWalk(std::uint32_t seed, int steps) {
    Mesh mesh = makeGrid(6, 6);
    std::mt19937 rng(seed);
    for (int step = 0; step < steps; ++step) {
        const auto edges = aliveEdges(mesh);
        const auto faces = aliveFaces(mesh);
        if (edges.empty() || faces.empty()) {
            break;
        }
        switch (rng() % 5) {
            case 0: {
                const EdgeId e = edges[rng() % edges.size()];
                mesh.splitEdge(e, 0.25f + 0.5f * static_cast<float>(rng() % 3) / 2.0f);
                break;
            }
            case 1: {
                const EdgeId e = edges[rng() % edges.size()];
                mesh.flipEdge(e);
                break;
            }
            case 2: {
                const EdgeId e = edges[rng() % edges.size()];
                mesh.collapseEdge(e);
                break;
            }
            case 3: {
                const FaceId f = faces[rng() % faces.size()];
                mesh.triangulateFace(f);
                break;
            }
            case 4: {
                const FaceId f = faces[rng() % faces.size()];
                mesh.removeFace(f);
                break;
            }
        }
        const auto errors = mesh.validate();
        if (!errors.empty()) {
            CAPTURE(seed);
            CAPTURE(step);
            CAPTURE(errors.front());
            REQUIRE(errors.empty());
        }
    }
}

}  // namespace

TEST_CASE("random operator walks keep every invariant") {
    for (std::uint32_t seed = 1; seed <= 12; ++seed) {
        runOperatorWalk(seed, 120);
    }
}

TEST_CASE("identical walks produce bitwise-identical meshes (determinism)") {
    auto runAndExport = [](std::uint32_t seed) {
        Mesh mesh = makeGrid(5, 5);
        std::mt19937 rng(seed);
        for (int step = 0; step < 60; ++step) {
            const auto edges = aliveEdges(mesh);
            if (edges.empty()) {
                break;
            }
            const EdgeId e = edges[rng() % edges.size()];
            switch (rng() % 3) {
                case 0:
                    mesh.splitEdge(e);
                    break;
                case 1:
                    mesh.flipEdge(e);
                    break;
                case 2:
                    mesh.collapseEdge(e);
                    break;
            }
        }
        std::vector<Vec3> positions;
        std::vector<std::vector<Index>> faces;
        mesh.toIndexed(positions, faces);
        return std::pair{positions, faces};
    };
    const auto [p1, f1] = runAndExport(42);
    const auto [p2, f2] = runAndExport(42);
    REQUIRE(p1.size() == p2.size());
    REQUIRE(f1 == f2);
    for (std::size_t i = 0; i < p1.size(); ++i) {
        REQUIRE(p1[i] == p2[i]);
    }
}
