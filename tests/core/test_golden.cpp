#include <doctest.h>

#include <cmath>
#include <string>
#include <vector>

#include "cyber/core/mesh.hpp"
#include "cyber/core/pipeline.hpp"

using cyber::Index;
using cyber::Mesh;
using cyber::Vec3;
using cyber::VertexId;
namespace remesh = cyber::remesh;

// Golden-mesh regression suite (remeshing-pipeline task 5.9). The corpus is
// procedural, so it is permissively licensed by construction and reproducible
// without external assets (an armadillo-class mesh can be dropped in later via
// the same harness). Rather than pin exact face counts — which would churn on
// every legitimate quadrangulator change, notably the pending QuadCover port
// (5.4) — the baselines assert the invariants a correct remesh must always
// hold: a manifold-valid, quad-dominant result at roughly the requested
// density, preserving the input's shape, produced deterministically.
namespace {

struct Bounds {
    Vec3 lo{1e30f, 1e30f, 1e30f};
    Vec3 hi{-1e30f, -1e30f, -1e30f};
    [[nodiscard]] Vec3 size() const { return hi - lo; }
};

Bounds boundsOf(const Mesh& mesh) {
    Bounds b;
    for (Index i = 0; i < mesh.vertexCapacity(); ++i) {
        if (!mesh.isAlive(VertexId{i})) {
            continue;
        }
        const Vec3 p = mesh.position(VertexId{i});
        b.lo = cyber::min(b.lo, p);
        b.hi = cyber::max(b.hi, p);
    }
    return b;
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

Mesh makeSphere(int rings, int segments) {
    std::vector<Vec3> p;
    p.push_back({0, 0, 1});
    for (int r = 1; r < rings; ++r) {
        const float phi = cyber::kPi * static_cast<float>(r) / static_cast<float>(rings);
        for (int s = 0; s < segments; ++s) {
            const float theta =
                2.0f * cyber::kPi * static_cast<float>(s) / static_cast<float>(segments);
            p.push_back(
                {std::sin(phi) * std::cos(theta), std::sin(phi) * std::sin(theta), std::cos(phi)});
        }
    }
    p.push_back({0, 0, -1});
    const Index south = static_cast<Index>(p.size() - 1);
    std::vector<std::vector<Index>> f;
    auto ring = [segments](int r, int s) {
        return static_cast<Index>(1 + (r - 1) * segments + (s % segments));
    };
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

// Genus-1 torus (major radius R, minor radius r) as an all-quad grid.
Mesh makeTorus(int major, int minor, float R, float r) {
    std::vector<Vec3> p;
    for (int i = 0; i < major; ++i) {
        const float u = 2.0f * cyber::kPi * static_cast<float>(i) / static_cast<float>(major);
        for (int j = 0; j < minor; ++j) {
            const float v = 2.0f * cyber::kPi * static_cast<float>(j) / static_cast<float>(minor);
            const float cu = std::cos(u), su = std::sin(u);
            const float cv = std::cos(v), sv = std::sin(v);
            p.push_back({(R + r * cv) * cu, (R + r * cv) * su, r * sv});
        }
    }
    auto idx = [&](int i, int j) { return static_cast<Index>((i % major) * minor + (j % minor)); };
    std::vector<std::vector<Index>> f;
    for (int i = 0; i < major; ++i) {
        for (int j = 0; j < minor; ++j) {
            f.push_back({idx(i, j), idx(i + 1, j), idx(i + 1, j + 1), idx(i, j + 1)});
        }
    }
    return Mesh::fromIndexed(p, f);
}

struct GoldenCase {
    std::string name;
    Mesh mesh;
    int targetQuads;
};

// Assert the invariant baselines a correct remesh output must satisfy.
void checkGolden(const GoldenCase& c) {
    CAPTURE(c.name);
    remesh::Parameters params;
    params.targetQuadCount = c.targetQuads;
    params.adaptivity = 1.0f;

    const remesh::PipelineResult result = remesh::remesh(c.mesh, params);
    REQUIRE(result.status == remesh::RunStatus::Success);
    REQUIRE(result.mesh.faceCount() > 0);

    // 1. Manifold-valid output.
    REQUIRE(result.mesh.validate().empty());

    // 2. Quad-dominant (spec: quad-dominant output; pure-quad is opt-in).
    //    0.55 is the recorded floor of the greedy-pairing baseline (the cube
    //    measures ~0.59, curved meshes higher); the QuadCover port (5.4) is
    //    expected to raise this — tighten the floor when it lands.
    const std::size_t faces =
        result.stats.quadCount + result.stats.triangleCount + result.stats.otherPolygonCount;
    REQUIRE(faces > 0);
    const double quadRatio =
        static_cast<double>(result.stats.quadCount) / static_cast<double>(faces);
    REQUIRE(quadRatio > 0.55);

    // 3. Density within a recorded band around the request (guards runaway
    //    refinement / collapse without pinning the exact algorithm output).
    REQUIRE(faces > static_cast<std::size_t>(c.targetQuads) / 8);
    REQUIRE(faces < static_cast<std::size_t>(c.targetQuads) * 8);

    // 4. Shape preserved: output bounds match input bounds closely.
    const Bounds in = boundsOf(c.mesh);
    const Bounds out = boundsOf(result.mesh);
    const Vec3 inSize = in.size();
    for (int axis = 0; axis < 3; ++axis) {
        const float a = (&inSize.x)[axis];
        const float ia = (&in.lo.x)[axis];
        const float oa = (&out.lo.x)[axis];
        const float ib = (&in.hi.x)[axis];
        const float ob = (&out.hi.x)[axis];
        REQUIRE(std::fabs(oa - ia) < a * 0.1f + 0.05f);
        REQUIRE(std::fabs(ob - ib) < a * 0.1f + 0.05f);
    }
}

std::vector<std::vector<Index>> topologyOf(const Mesh& mesh) {
    std::vector<Vec3> positions;
    std::vector<std::vector<Index>> faces;
    mesh.toIndexed(positions, faces);
    return faces;
}

}  // namespace

TEST_CASE("golden corpus: cube remeshes to a valid quad-dominant result") {
    checkGolden({"cube", makeCube(), 400});
}

TEST_CASE("golden corpus: sphere remeshes to a valid quad-dominant result") {
    checkGolden({"sphere", makeSphere(16, 24), 1500});
}

TEST_CASE("golden corpus: torus (genus 1) remeshes to a valid quad-dominant result") {
    checkGolden({"torus", makeTorus(48, 24, 1.0f, 0.35f), 2000});
}

TEST_CASE("golden corpus: the pipeline is deterministic (identical topology across runs)") {
    // The recorded baseline for determinism is exact: same input + params must
    // yield byte-identical face topology every run (spec: deterministic order).
    const Mesh sphere = makeSphere(16, 24);
    remesh::Parameters params;
    params.targetQuadCount = 1500;
    params.adaptivity = 1.0f;

    const auto a = topologyOf(remesh::remesh(sphere, params).mesh);
    const auto b = topologyOf(remesh::remesh(sphere, params).mesh);
    REQUIRE(a == b);
}

TEST_CASE("golden corpus: pure-quad mode yields an all-quad result") {
    const Mesh sphere = makeSphere(16, 24);
    remesh::Parameters params;
    params.targetQuadCount = 1500;
    params.pureQuads = true;

    const remesh::PipelineResult result = remesh::remesh(sphere, params);
    REQUIRE(result.status == remesh::RunStatus::Success);
    REQUIRE(result.mesh.validate().empty());
    REQUIRE(result.stats.quadCount > 0);
    REQUIRE(result.stats.triangleCount == 0);
    REQUIRE(result.stats.otherPolygonCount == 0);
}
