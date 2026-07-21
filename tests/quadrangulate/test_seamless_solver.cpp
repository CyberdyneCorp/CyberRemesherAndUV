#include <doctest.h>

#include <cmath>
#include <cstddef>
#include <map>
#include <tuple>
#include <vector>

#include "cyber/accel/backend.hpp"
#include "cyber/core/mesh.hpp"
#include "cyber/quadrangulate/quadcover_extractor.hpp"
#include "cyber/quadrangulate/seamless_solver.hpp"

using cyber::EdgeId;
using cyber::FaceId;
using cyber::Index;
using cyber::Mesh;
using cyber::Vec2;
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

// An OPEN bowl: the northern cap of the sphere (rings 0..half), left open at the equator so its
// whole equatorial ring is a genuine boundary. Exercises the free-boundary path where the ARAP
// distortion polish is gated OFF (a shifted free boundary has no downstream cleanup safety net):
// the solve must still return a valid, BOUNDED parameterization.
Mesh makeOpenBowl(int rings = 20, int segments = 30) {
    const int half = rings / 2;
    std::vector<Vec3> p;
    p.push_back({0, 0, 1});
    for (int r = 1; r <= half; ++r) {
        const float phi = 3.14159265f * static_cast<float>(r) / static_cast<float>(rings);
        for (int s = 0; s < segments; ++s) {
            const float th = 2.0f * 3.14159265f * static_cast<float>(s) / static_cast<float>(segments);
            p.push_back({std::sin(phi) * std::cos(th), std::sin(phi) * std::sin(th), std::cos(phi)});
        }
    }
    const auto ring = [&](int r, int s) {
        return static_cast<Index>(1 + (r - 1) * segments + (s % segments));
    };
    std::vector<std::vector<Index>> f;
    for (int s = 0; s < segments; ++s) {
        f.push_back({0, ring(1, s), ring(1, s + 1)});
    }
    for (int r = 1; r < half; ++r) {
        for (int s = 0; s < segments; ++s) {
            f.push_back({ring(r, s), ring(r + 1, s), ring(r + 1, s + 1)});
            f.push_back({ring(r, s), ring(r + 1, s + 1), ring(r, s + 1)});
        }
    }
    return Mesh::fromIndexed(p, f);
}

// A subdivided cube projected to the unit sphere. Its 8 corners concentrate curvature into
// EIGHT index-1 cross-field cones (sum 8 == 4*chi), so the cut graph is a branching tree over
// the cones — the many-singularity case whose branch-point holonomy must be reconciled for the
// seam to close (a single-path cut like the plain sphere never exercises it).
Mesh makeCubeSphere(int n = 8) {
    std::vector<Vec3> p;
    std::vector<std::vector<Index>> f;
    std::map<std::tuple<int, int, int>, Index> idx;
    const auto add = [&](const Vec3& raw) -> Index {
        const Vec3 u = normalized(raw);
        const std::tuple<int, int, int> k{static_cast<int>(std::lround(u.x * 1000.0f)),
                                          static_cast<int>(std::lround(u.y * 1000.0f)),
                                          static_cast<int>(std::lround(u.z * 1000.0f))};
        const auto it = idx.find(k);
        if (it != idx.end()) {
            return it->second;
        }
        const Index id = static_cast<Index>(p.size());
        p.push_back(u);
        idx.emplace(k, id);
        return id;
    };
    const int dirs[6][3] = {{0, 0, 1}, {0, 0, -1}, {0, 1, 0}, {0, -1, 0}, {1, 0, 0}, {-1, 0, 0}};
    for (const auto& d : dirs) {
        const Vec3 nrm{static_cast<float>(d[0]), static_cast<float>(d[1]), static_cast<float>(d[2])};
        const Vec3 a = std::abs(nrm.z) < 0.5f ? Vec3{0, 0, 1} : Vec3{1, 0, 0};
        const Vec3 t1 = normalized(cross(nrm, a));
        const Vec3 t2 = cross(nrm, t1);
        const auto pt = [&](int i, int j) {
            const float u = -1.0f + 2.0f * static_cast<float>(i) / static_cast<float>(n);
            const float v = -1.0f + 2.0f * static_cast<float>(j) / static_cast<float>(n);
            return nrm + t1 * u + t2 * v;
        };
        for (int i = 0; i < n; ++i) {
            for (int j = 0; j < n; ++j) {
                const Index a0 = add(pt(i, j)), a1 = add(pt(i + 1, j));
                const Index a2 = add(pt(i + 1, j + 1)), a3 = add(pt(i, j + 1));
                f.push_back({a0, a1, a2});
                f.push_back({a0, a2, a3});
            }
        }
    }
    return Mesh::fromIndexed(p, f);
}

// A flat-faced cube tessellated n x n per side (welded along the 12 cube edges). Unlike the
// cube-SPHERE, the faces stay planar so the 12 cube edges are genuine 90-degree creases: after
// tagFeatureEdges they are feature edges. This is the CAD / sharp-feature case that used to be
// declined by the native solver (and, once run, blew the integer map up); it exercises the
// feature-as-seam path and the per-component gauge pinning that keeps the map bounded.
Mesh makeCube(int n = 6) {
    std::vector<Vec3> p;
    std::vector<std::vector<Index>> f;
    std::map<std::tuple<int, int, int>, Index> idx;
    const auto add = [&](const Vec3& raw) -> Index {
        const std::tuple<int, int, int> k{static_cast<int>(std::lround(raw.x * 1000.0f)),
                                          static_cast<int>(std::lround(raw.y * 1000.0f)),
                                          static_cast<int>(std::lround(raw.z * 1000.0f))};
        const auto it = idx.find(k);
        if (it != idx.end()) {
            return it->second;
        }
        const Index id = static_cast<Index>(p.size());
        p.push_back(raw);
        idx.emplace(k, id);
        return id;
    };
    const int dirs[6][3] = {{0, 0, 1}, {0, 0, -1}, {0, 1, 0}, {0, -1, 0}, {1, 0, 0}, {-1, 0, 0}};
    for (const auto& d : dirs) {
        const Vec3 nrm{static_cast<float>(d[0]), static_cast<float>(d[1]), static_cast<float>(d[2])};
        const Vec3 a = std::abs(nrm.z) < 0.5f ? Vec3{0, 0, 1} : Vec3{1, 0, 0};
        const Vec3 t1 = normalized(cross(nrm, a));
        const Vec3 t2 = cross(nrm, t1);
        const auto pt = [&](int i, int j) {
            const float u = -1.0f + 2.0f * static_cast<float>(i) / static_cast<float>(n);
            const float v = -1.0f + 2.0f * static_cast<float>(j) / static_cast<float>(n);
            return nrm + t1 * u + t2 * v;
        };
        for (int i = 0; i < n; ++i) {
            for (int j = 0; j < n; ++j) {
                const Index a0 = add(pt(i, j)), a1 = add(pt(i + 1, j));
                const Index a2 = add(pt(i + 1, j + 1)), a3 = add(pt(i, j + 1));
                f.push_back({a0, a1, a2});
                f.push_back({a0, a2, a3});
            }
        }
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

namespace {

// Assemble a SeamlessUv on the input triangles from a per-corner parameterization, so the
// integer-jump residual metric can score it.
remesh::SeamlessUv assembleUv(const Mesh& mesh, const remesh::Parameterization& param) {
    remesh::SeamlessUv uv;
    uv.vertices.assign(mesh.vertexCapacity(), Vec3{0, 0, 0});
    for (Index vi = 0; vi < mesh.vertexCapacity(); ++vi) {
        if (mesh.isAlive(VertexId{vi})) uv.vertices[vi] = mesh.position(VertexId{vi});
    }
    for (Index fi = 0; fi < mesh.faceCapacity(); ++fi) {
        const FaceId f{fi};
        if (!mesh.isAlive(f) || mesh.faceSize(f) != 3) continue;
        const std::vector<VertexId> vs = mesh.faceVertices(f);
        uv.triangles.push_back({vs[0].value, vs[1].value, vs[2].value});
        uv.triangleUv.push_back(param.cornerUv[fi]);
    }
    uv.valid = !uv.triangles.empty();
    return uv;
}

}  // namespace

// Milestone 2b/c (integer-seamless): the constrained solve makes the seam a rigid integer
// grid symmetry. The acceptance gate is the same metric that validated the vendored solver —
// seamlessUvResidual (max integer-jump residual over interior edges) must be ~0. A relaxed
// solve leaves it ~0.5; the KKT/rigidity + integer-rounding phase must drive it below 1e-3.
TEST_CASE("seamless M2c: constrained solve is integer-seamless on a sphere (residual < 1e-3)") {
    auto backend = cyber::accel::defaultBackend();
    const Mesh sphere = makeSphere();
    const remesh::SeamlessSetup setup = remesh::buildSeamlessSetup(sphere, 50, *backend);
    REQUIRE(setup.valid);

    const remesh::Parameterization param =
        remesh::solveParameterization(sphere, setup, 0.12f, *backend);
    REQUIRE(param.valid);

    const remesh::SeamlessUv uv = assembleUv(sphere, param);
    REQUIRE(uv.valid);
    CHECK(remesh::seamlessUvResidual(uv) < 1e-3);

    // The seam being rigid, the extractor traces a bounded, clean quad mesh (a non-rigid seam
    // blows the UV up into hundreds of thousands of spurious cells).
    const remesh::IsolineQuadMesh quads = remesh::extractIsolineQuads(sphere, uv);
    CHECK(quads.quads.size() > 100);
    CHECK(quads.quads.size() < 2000);
}

// The ARAP distortion polish is GATED OFF on an open (free-boundary) surface, where a shifted
// boundary has no downstream extractor cleanup to fall back on. This guards that the gated path
// still returns a valid, BOUNDED parameterization (the divergence failure mode) on a genuinely
// open surface, rather than being destabilised by a boundary-hugging polish.
TEST_CASE("seamless ARAP gate: open bowl stays valid and bounded (polish skipped)") {
    auto backend = cyber::accel::defaultBackend();
    const Mesh bowl = makeOpenBowl();
    const remesh::SeamlessSetup setup = remesh::buildSeamlessSetup(bowl, 50, *backend);
    REQUIRE(setup.valid);

    const remesh::Parameterization param =
        remesh::solveParameterization(bowl, setup, 0.12f, *backend);
    REQUIRE(param.valid);

    // Every corner UV is finite and within a sane grid range: the target count implies O(10s) of
    // isolines, so a correct bounded map stays well under a few hundred cells across (a divergent
    // map, the failure this guards, blows up to 1e4+ before the magnitude cap even engages).
    float maxAbs = 0.0f;
    for (Index fi = 0; fi < bowl.faceCapacity(); ++fi) {
        if (!bowl.isAlive(FaceId{fi}) || bowl.faceSize(FaceId{fi}) != 3) continue;
        for (const cyber::Vec2& c : param.cornerUv[fi]) {
            REQUIRE(std::isfinite(c.x));
            REQUIRE(std::isfinite(c.y));
            maxAbs = std::max(maxAbs, std::max(std::abs(c.x), std::abs(c.y)));
        }
    }
    CHECK(maxAbs > 1.0f);    // a non-degenerate map spanning several isolines
    CHECK(maxAbs < 500.0f);  // bounded, not divergent
}

// Milestone 2c (many-singularity / branch-point holonomy): the sparse constraint-elimination
// solve must stay seamless when the cut graph BRANCHES. The cube-sphere has eight cross-field
// cones, so the cut is a branching tree and the integer transitions must be reconciled around
// every junction (the earlier dense-dual path dropped one weld per cycle and left residual
// ~0.5). The gate is the same: seamlessUvResidual ~0 AND a real quad-dominant extraction.
TEST_CASE("seamless M2c: branching-cut many-cone surface stays seamless (residual < 1e-3)") {
    auto backend = cyber::accel::defaultBackend();
    const Mesh cube = makeCubeSphere(8);
    const remesh::SeamlessSetup setup = remesh::buildSeamlessSetup(cube, 50, *backend);
    REQUIRE(setup.valid);
    // Eight cones (Poincare-Hopf sum 8) -> a branching cut tree, not a single path.
    CHECK(setup.totalIndex() == 8);
    CHECK(setup.singularityCount() >= 4);

    const remesh::Parameterization param =
        remesh::solveParameterization(cube, setup, 0.12f, *backend);
    REQUIRE(param.valid);

    const remesh::SeamlessUv uv = assembleUv(cube, param);
    REQUIRE(uv.valid);
    CHECK(remesh::seamlessUvResidual(uv) < 1e-3);

    // A quad-DOMINANT extraction: hundreds of cells, the clear majority valence-4.
    const remesh::IsolineQuadMesh quads = remesh::extractIsolineQuads(cube, uv);
    CHECK(quads.quads.size() > 100);
    std::size_t nQuad = 0;
    for (const auto& q : quads.quads) {
        if (q.size() == 4) {
            ++nQuad;
        }
    }
    CHECK(nQuad * 2 > quads.quads.size());  // > 50% are quads
}

namespace {

// Max |UV| over all corners — a divergence probe. A sane integer-grid map spans O(grid extent)
// cells; a divergent one explodes to 1e6+.
float maxAbsUv(const Mesh& mesh, const remesh::Parameterization& param) {
    float m = 0.0f;
    for (Index fi = 0; fi < mesh.faceCapacity(); ++fi) {
        if (!mesh.isAlive(FaceId{fi}) || mesh.faceSize(FaceId{fi}) != 3) continue;
        for (const Vec2& c : param.cornerUv[fi]) {
            m = std::max(m, std::max(std::abs(c.x), std::abs(c.y)));
        }
    }
    return m;
}

}  // namespace

// Regression (feature meshes RUN natively): a sharp-edged cube used to be declined by the native
// solver — its feature edges were left as dead interior edges, so the integer map was
// ill-conditioned and blew up. buildSeamlessSetup must now mark the 12 creases as cut (seam)
// edges, and solveParameterization must produce a valid, BOUNDED (non-divergent) integer-seamless
// map. This is the gate that lets CAD / sharp-feature models take the native path.
TEST_CASE("seamless feature: sharp cube runs and stays bounded (feature edges become seams)") {
    auto backend = cyber::accel::defaultBackend();
    Mesh cube = makeCube(6);
    // A 90-degree cube crease has a 90-degree normal angle, so it registers as a feature only
    // above a ~90-degree dihedral threshold (tagFeatureEdges: feature when normalAngle >=
    // 180 - threshold). Use 120 so the 12 cube edges are tagged.
    cube.tagFeatureEdges(120.0f);

    // Some interior edges are tagged as 90-degree creases.
    std::size_t featureEdges = 0;
    for (Index ei = 0; ei < cube.edgeCapacity(); ++ei) {
        if (cube.isAlive(EdgeId{ei}) && cube.edgeFaceCount(EdgeId{ei}) == 2 &&
            cube.isFeatureEdge(EdgeId{ei})) {
            ++featureEdges;
        }
    }
    REQUIRE(featureEdges > 0);

    const remesh::SeamlessSetup setup = remesh::buildSeamlessSetup(cube, 50, *backend);
    REQUIRE(setup.valid);

    // Every interior feature edge is marked as a cut (seam) edge.
    for (Index ei = 0; ei < cube.edgeCapacity(); ++ei) {
        if (cube.isAlive(EdgeId{ei}) && cube.edgeFaceCount(EdgeId{ei}) == 2 &&
            cube.isFeatureEdge(EdgeId{ei})) {
            CHECK(setup.isCutEdge[ei]);
        }
    }

    const remesh::Parameterization param =
        remesh::solveParameterization(cube, setup, 0.25f, *backend);
    REQUIRE(param.valid);

    // The map is BOUNDED — the per-component gauge pinning kept it from diverging (pre-fix this
    // exploded to ~1e6+). A sane map on a unit cube spans only a handful of grid cells.
    CHECK(maxAbsUv(cube, param) < 1e3f);
    // And it is integer-seamless across the (now feature-aware) seams.
    const remesh::SeamlessUv uv = assembleUv(cube, param);
    REQUIRE(uv.valid);
    CHECK(remesh::seamlessUvResidual(uv) < 1e-3);
}

// Regression (cooperative cancellation): a token cancelled before the solve must make
// solveParameterization return promptly with an INVALID parameterization (so computeSeamlessUv
// degrades cleanly) rather than running the full CG + integer rounding to completion.
TEST_CASE("seamless cancel: pre-cancelled token aborts the solve with an invalid result") {
    auto backend = cyber::accel::defaultBackend();
    const Mesh sphere = makeSphere();
    const remesh::SeamlessSetup setup = remesh::buildSeamlessSetup(sphere, 30, *backend);
    REQUIRE(setup.valid);

    cyber::CancelToken cancel;
    cancel.requestCancel();
    const remesh::Parameterization param =
        remesh::solveParameterization(sphere, setup, 0.12f, *backend, &cancel);
    CHECK_FALSE(param.valid);
}
