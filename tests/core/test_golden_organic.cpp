#include <doctest.h>

#include <cmath>
#include <cstddef>
#include <string>
#include <vector>

#include "cyber/core/mesh.hpp"
#include "cyber/core/pipeline.hpp"

using cyber::Index;
using cyber::Mesh;
using cyber::Vec3;
using cyber::VertexId;
namespace remesh = cyber::remesh;

// Golden-mesh regression suite, organic corpus (remeshing-pipeline task 5.9).
// The base corpus (test_golden.cpp) exercises analytic primitives — cube,
// sphere, torus — whose curvature is uniform and whose feature structure is
// trivial. Real remesh inputs are neither: they are lumpy, high-genus, and
// have curvature that swings across the surface. This file adds two such
// procedural, asset-free models so the invariant baselines are stressed by
// geometry closer to production:
//
//   * a dimpled sphere — radius modulated by a sum of sines of (theta, phi),
//     so the isotropic stage must chase spatially varying curvature; and
//   * a (2,3) torus-knot (trefoil) tube built with a parallel-transport
//     frame — a genus-1 surface that folds back on itself, stressing island
//     handling and orientation without self-intersecting at the chosen tube
//     radius.
//
// As in the base suite the assertions are invariants, not pinned face counts:
// a correct remesh must always land on a manifold-valid, quad-dominant result
// near the requested density, preserve the input's shape, and be
// deterministic. The quad-ratio and density floors are recorded conservatively
// below the greedy-pairing baseline so they survive the pending QuadCover port
// (5.4); tighten them when it lands.
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

// Star-shaped closed surface: unit sphere whose radius is displaced by a sum
// of low-frequency sines of (theta, phi). The displacement amplitude keeps the
// radius strictly positive, so the surface stays a valid closed manifold
// (identical topology to a UV sphere) while presenting spatially varying
// curvature to the isotropic stage.
float dimpleRadius(float phi, float theta) {
    return 1.0f + 0.18f * std::sin(3.0f * theta) * std::sin(2.0f * phi) +
           0.12f * std::cos(4.0f * phi) + 0.08f * std::sin(5.0f * theta);
}

Mesh makeDimpledSphere(int rings, int segments) {
    std::vector<Vec3> p;
    // North pole: radius uses phi = 0 (theta is irrelevant at the pole).
    p.push_back({0, 0, dimpleRadius(0.0f, 0.0f)});
    for (int r = 1; r < rings; ++r) {
        const float phi = cyber::kPi * static_cast<float>(r) / static_cast<float>(rings);
        for (int s = 0; s < segments; ++s) {
            const float theta =
                2.0f * cyber::kPi * static_cast<float>(s) / static_cast<float>(segments);
            const float rad = dimpleRadius(phi, theta);
            p.push_back({rad * std::sin(phi) * std::cos(theta),
                         rad * std::sin(phi) * std::sin(theta), rad * std::cos(phi)});
        }
    }
    const float southPhi = cyber::kPi;
    p.push_back({0, 0, dimpleRadius(southPhi, 0.0f) * std::cos(southPhi)});
    const Index south = static_cast<Index>(p.size() - 1);

    auto ring = [segments](int r, int s) {
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

// Rodrigues rotation of `v` around unit `axis` by the angle whose cosine/sine
// are (c, s).
Vec3 rotateAroundAxis(Vec3 v, Vec3 axis, float c, float s) {
    return v * c + cyber::cross(axis, v) * s + axis * (cyber::dot(axis, v) * (1.0f - c));
}

// Classic (2,3) torus knot (trefoil) space curve.
Vec3 trefoilPoint(float t) {
    return {std::sin(t) + 2.0f * std::sin(2.0f * t), std::cos(t) - 2.0f * std::cos(2.0f * t),
            -std::sin(3.0f * t)};
}

// Tube of radius `tube` swept along the trefoil with a parallel-transport
// (rotation-minimizing) frame, closed in both directions into an all-quad
// genus-1 surface. Any framing holonomy at the seam leaves the surface a valid
// closed manifold — the seam quads are merely slightly sheared, never
// degenerate — which is exactly the kind of near-feature irregularity the
// remesh must digest.
Mesh makeTrefoilTube(int major, int minor, float tube) {
    std::vector<Vec3> centers(static_cast<std::size_t>(major));
    for (int i = 0; i < major; ++i) {
        const float t = 2.0f * cyber::kPi * static_cast<float>(i) / static_cast<float>(major);
        centers[static_cast<std::size_t>(i)] = trefoilPoint(t);
    }

    auto at = [&](int i) { return centers[static_cast<std::size_t>((i % major + major) % major)]; };

    std::vector<Vec3> tangent(static_cast<std::size_t>(major));
    for (int i = 0; i < major; ++i) {
        tangent[static_cast<std::size_t>(i)] = cyber::normalized(at(i + 1) - at(i - 1));
    }

    std::vector<Vec3> frameN(static_cast<std::size_t>(major));
    std::vector<Vec3> frameB(static_cast<std::size_t>(major));
    // Seed a normal perpendicular to the first tangent, chosen from whichever
    // reference axis is least parallel to it.
    const Vec3 t0 = tangent[0];
    const Vec3 ref = (std::fabs(t0.x) < 0.9f) ? Vec3{1, 0, 0} : Vec3{0, 1, 0};
    frameN[0] = cyber::normalized(cyber::cross(t0, ref));
    frameB[0] = cyber::cross(t0, frameN[0]);
    for (int i = 1; i < major; ++i) {
        const Vec3 prevT = tangent[static_cast<std::size_t>(i - 1)];
        const Vec3 curT = tangent[static_cast<std::size_t>(i)];
        const Vec3 axis = cyber::cross(prevT, curT);
        const float s = cyber::length(axis);
        Vec3 n = frameN[static_cast<std::size_t>(i - 1)];
        if (s > 1e-6f) {
            const float c = cyber::dot(prevT, curT);
            n = rotateAroundAxis(n, axis / s, c, s);
        }
        // Re-orthogonalise against the current tangent to fight drift.
        n = cyber::normalized(n - curT * cyber::dot(n, curT));
        frameN[static_cast<std::size_t>(i)] = n;
        frameB[static_cast<std::size_t>(i)] = cyber::cross(curT, n);
    }

    std::vector<Vec3> p;
    p.reserve(static_cast<std::size_t>(major) * static_cast<std::size_t>(minor));
    for (int i = 0; i < major; ++i) {
        const Vec3 c = centers[static_cast<std::size_t>(i)];
        const Vec3 n = frameN[static_cast<std::size_t>(i)];
        const Vec3 b = frameB[static_cast<std::size_t>(i)];
        for (int j = 0; j < minor; ++j) {
            const float v = 2.0f * cyber::kPi * static_cast<float>(j) / static_cast<float>(minor);
            p.push_back(c + (n * (tube * std::cos(v)) + b * (tube * std::sin(v))));
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

std::vector<std::vector<Index>> topologyOf(const Mesh& mesh) {
    std::vector<Vec3> positions;
    std::vector<std::vector<Index>> faces;
    mesh.toIndexed(positions, faces);
    return faces;
}

void toIndexedBoth(const Mesh& mesh, std::vector<Vec3>& positions,
                   std::vector<std::vector<Index>>& faces) {
    mesh.toIndexed(positions, faces);
}

// Assert the invariant baselines for one organic input at one requested
// density.
void checkOrganic(const std::string& name, const Mesh& input, int targetQuads) {
    CAPTURE(name);
    CAPTURE(targetQuads);
    // Sanity: the generated input is itself a valid closed manifold, otherwise
    // a downstream failure would be ambiguous.
    REQUIRE(input.faceCount() > 0);
    REQUIRE(input.validate().empty());

    remesh::Parameters params;
    params.targetQuadCount = targetQuads;
    params.adaptivity = 1.0f;

    const remesh::PipelineResult result = remesh::remesh(input, params);
    REQUIRE(result.status == remesh::RunStatus::Success);
    REQUIRE(result.mesh.faceCount() > 0);

    // 1. Manifold-valid output.
    REQUIRE(result.mesh.validate().empty());

    // 2. Quad-dominant. 0.55 is the recorded floor of the greedy-pairing
    //    baseline shared with the analytic corpus; curved organic inputs
    //    measure comfortably above it. Tighten when the QuadCover port lands.
    const std::size_t faces =
        result.stats.quadCount + result.stats.triangleCount + result.stats.otherPolygonCount;
    REQUIRE(faces > 0);
    const double quadRatio =
        static_cast<double>(result.stats.quadCount) / static_cast<double>(faces);
    REQUIRE(quadRatio > 0.55);

    // 3. Density within a recorded band around the request (guards runaway
    //    refinement / collapse without pinning the exact algorithm output).
    REQUIRE(faces > static_cast<std::size_t>(targetQuads) / 8);
    REQUIRE(faces < static_cast<std::size_t>(targetQuads) * 8);

    // 4. Shape preserved: output bounds match input bounds closely. The
    //    tolerance is a hair looser than the analytic corpus's because organic
    //    extrema (dimple tips, tube seams) round slightly under resampling.
    const Bounds in = boundsOf(input);
    const Bounds out = boundsOf(result.mesh);
    const Vec3 inSize = in.size();
    for (int axis = 0; axis < 3; ++axis) {
        const float a = (&inSize.x)[axis];
        const float ia = (&in.lo.x)[axis];
        const float oa = (&out.lo.x)[axis];
        const float ib = (&in.hi.x)[axis];
        const float ob = (&out.hi.x)[axis];
        REQUIRE(std::fabs(oa - ia) < a * 0.12f + 0.06f);
        REQUIRE(std::fabs(ob - ib) < a * 0.12f + 0.06f);
    }
}

}  // namespace

TEST_CASE("golden organic: dimpled sphere remeshes to a valid quad-dominant result") {
    const Mesh mesh = makeDimpledSphere(20, 30);
    // A couple of target densities exercise both coarser and finer resampling.
    checkOrganic("dimpled-sphere-coarse", mesh, 1200);
    checkOrganic("dimpled-sphere-fine", mesh, 3000);
}

TEST_CASE("golden organic: trefoil-knot tube remeshes to a valid quad-dominant result") {
    const Mesh mesh = makeTrefoilTube(160, 16, 0.55f);
    checkOrganic("trefoil-tube-coarse", mesh, 1500);
    checkOrganic("trefoil-tube-fine", mesh, 3500);
}

TEST_CASE("golden organic: the pipeline is deterministic on an organic input") {
    // Determinism baseline is exact: same input + params must yield
    // byte-identical positions and face topology every run.
    const Mesh mesh = makeTrefoilTube(160, 16, 0.55f);
    remesh::Parameters params;
    params.targetQuadCount = 2000;
    params.adaptivity = 1.0f;

    std::vector<Vec3> posA;
    std::vector<std::vector<Index>> facesA;
    std::vector<Vec3> posB;
    std::vector<std::vector<Index>> facesB;
    toIndexedBoth(remesh::remesh(mesh, params).mesh, posA, facesA);
    toIndexedBoth(remesh::remesh(mesh, params).mesh, posB, facesB);
    REQUIRE(facesA == facesB);
    REQUIRE(posA == posB);
}

TEST_CASE("golden organic: pure-quad mode yields an all-quad organic result") {
    const Mesh mesh = makeDimpledSphere(20, 30);
    remesh::Parameters params;
    params.targetQuadCount = 2000;
    params.pureQuads = true;

    const remesh::PipelineResult result = remesh::remesh(mesh, params);
    REQUIRE(result.status == remesh::RunStatus::Success);
    REQUIRE(result.mesh.validate().empty());
    REQUIRE(result.stats.quadCount > 0);
    REQUIRE(result.stats.triangleCount == 0);
    REQUIRE(result.stats.otherPolygonCount == 0);
    // Determinism holds in pure-quad mode too.
    REQUIRE(topologyOf(result.mesh) == topologyOf(remesh::remesh(mesh, params).mesh));
}
