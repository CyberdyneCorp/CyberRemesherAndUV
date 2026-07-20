#include <doctest.h>

#include <cmath>
#include <cstddef>
#include <vector>

#include "cyber/accel/backend.hpp"
#include "cyber/core/mesh.hpp"
#include "cyber/quadrangulate/seamless_solver.hpp"

using cyber::EdgeId;
using cyber::FaceId;
using cyber::Index;
using cyber::Mesh;
using cyber::Vec3;
using cyber::VertexId;
namespace remesh = cyber::remesh;

namespace {

Mesh makeSphere(int rings = 20, int segments = 30) {
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

Mesh makeTorus(int major = 40, int minor = 20) {
    std::vector<Vec3> p;
    const float R0 = 1.0f, r0 = 0.35f;
    for (int i = 0; i < major; ++i) {
        const float u = 2.0f * 3.14159265f * static_cast<float>(i) / static_cast<float>(major);
        for (int j = 0; j < minor; ++j) {
            const float v = 2.0f * 3.14159265f * static_cast<float>(j) / static_cast<float>(minor);
            p.push_back({(R0 + r0 * std::cos(v)) * std::cos(u),
                         (R0 + r0 * std::cos(v)) * std::sin(u), r0 * std::sin(v)});
        }
    }
    const auto id = [&](int i, int j) {
        return static_cast<Index>((i % major) * minor + (j % minor));
    };
    std::vector<std::vector<Index>> f;
    for (int i = 0; i < major; ++i) {
        for (int j = 0; j < minor; ++j) {
            f.push_back({id(i, j), id(i + 1, j), id(i + 1, j + 1)});
            f.push_back({id(i, j), id(i + 1, j + 1), id(i, j + 1)});
        }
    }
    return Mesh::fromIndexed(p, f);
}

int eulerCharacteristic(const Mesh& m) {
    std::size_t V = 0, E = 0, F = 0;
    for (Index i = 0; i < m.vertexCapacity(); ++i) {
        if (m.isAlive(VertexId{i})) ++V;
    }
    for (Index i = 0; i < m.edgeCapacity(); ++i) {
        if (m.isAlive(EdgeId{i})) ++E;
    }
    for (Index i = 0; i < m.faceCapacity(); ++i) {
        if (m.isAlive(FaceId{i})) ++F;
    }
    return static_cast<int>(V) - static_cast<int>(E) + static_cast<int>(F);
}

}  // namespace

// Native seamless-UV Milestone 1: the frame-field setup. The cross-field singularity
// indices are a topological invariant — by Poincare-Hopf they must sum to 4 * Euler
// characteristic regardless of the mesh or field, which is the rigorous correctness gate
// for the period-jump + index computation.
TEST_CASE("seamless M1: singularity indices satisfy Poincare-Hopf (sum == 4*chi)") {
    auto backend = cyber::accel::defaultBackend();

    SUBCASE("sphere (genus 0, chi 2 -> sum 8)") {
        const Mesh sphere = makeSphere();
        const remesh::SeamlessSetup setup = remesh::buildSeamlessSetup(sphere, 30, *backend);
        REQUIRE(setup.valid);
        CHECK(setup.totalIndex() == 4 * eulerCharacteristic(sphere));
        CHECK(setup.totalIndex() == 8);
        CHECK(setup.singularityCount() > 0);
        // Per-edge / per-vertex tables are sized to the mesh.
        CHECK(setup.periodJump.size() == sphere.edgeCapacity());
        CHECK(setup.singularityIndex.size() == sphere.vertexCapacity());
    }

    SUBCASE("torus (genus 1, chi 0 -> sum 0)") {
        const Mesh torus = makeTorus();
        const remesh::SeamlessSetup setup = remesh::buildSeamlessSetup(torus, 30, *backend);
        REQUIRE(setup.valid);
        CHECK(setup.totalIndex() == 4 * eulerCharacteristic(torus));
        CHECK(setup.totalIndex() == 0);
    }
}

// The cut graph slits a closed genus-0 surface open into a disk (Euler characteristic 1),
// which is the domain the seamless parameterization (M2) is solved on.
TEST_CASE("seamless M1: cut graph opens a genus-0 surface to a disk (chi == 1)") {
    auto backend = cyber::accel::defaultBackend();
    const Mesh sphere = makeSphere();
    const remesh::SeamlessSetup setup = remesh::buildSeamlessSetup(sphere, 30, *backend);
    REQUIRE(setup.valid);
    CHECK(eulerCharacteristic(sphere) == 2);  // closed sphere
    CHECK(remesh::cutOpenEulerCharacteristic(sphere, setup) == 1);  // cut open -> disk
}

// An empty mesh yields an invalid setup (clean degrade).
TEST_CASE("seamless M1: empty mesh -> invalid setup") {
    auto backend = cyber::accel::defaultBackend();
    const Mesh empty;
    const remesh::SeamlessSetup setup = remesh::buildSeamlessSetup(empty, 10, *backend);
    CHECK_FALSE(setup.valid);
}

namespace {

std::size_t aliveVertices(const Mesh& m) {
    std::size_t n = 0;
    for (Index i = 0; i < m.vertexCapacity(); ++i) {
        if (m.isAlive(VertexId{i})) ++n;
    }
    return n;
}

}  // namespace

// Native seamless-UV Milestone 2 (relaxed solve): the cotangent Poisson solve on the mesh
// cut open along the seam. A closed surface cannot carry a globally continuous integer-grid
// UV, so the solve runs on the cut mesh with duplicated seam vertices — the UV jumps by a
// grid symmetry across the seam. This is the RELAXED solve (real-valued), so the seam
// translations are not yet integers (residual > 0); integer rounding is M2c.
TEST_CASE("seamless M2: relaxed Poisson solve produces a cut-mesh parameterization") {
    auto backend = cyber::accel::defaultBackend();
    const Mesh sphere = makeSphere();
    const remesh::SeamlessSetup setup = remesh::buildSeamlessSetup(sphere, 50, *backend);
    REQUIRE(setup.valid);

    const remesh::Parameterization param =
        remesh::solveParameterization(sphere, setup, 0.12f, *backend);
    REQUIRE(param.valid);

    // The cut duplicated seam vertices: the solve mesh has more vertices than the input.
    CHECK(static_cast<std::size_t>(param.cutVertexCount) > aliveVertices(sphere));
    // CG converged (well under the iteration cap).
    CHECK(param.cgIterationsU > 0);
    CHECK(param.cgIterationsV > 0);
    // Per-corner UV was produced for the alive faces.
    CHECK(param.cornerUv.size() == sphere.faceCapacity());

    // The relaxed UV is a genuine (if not-yet-integer-seamless) parameterization: the
    // gradient is non-degenerate, so the corner UVs actually vary across the mesh.
    float uMin = 1e30f, uMax = -1e30f;
    for (Index fi = 0; fi < sphere.faceCapacity(); ++fi) {
        if (!sphere.isAlive(FaceId{fi}) || sphere.faceSize(FaceId{fi}) != 3) continue;
        for (const cyber::Vec2& c : param.cornerUv[fi]) {
            uMin = std::min(uMin, c.x);
            uMax = std::max(uMax, c.x);
        }
    }
    CHECK(uMax - uMin > 1.0f);  // spans several integer isolines
}
