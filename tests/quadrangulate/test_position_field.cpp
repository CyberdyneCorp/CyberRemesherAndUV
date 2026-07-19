#include <doctest.h>

#include <cmath>
#include <vector>

#include "cyber/core/math.hpp"
#include "cyber/core/mesh.hpp"
#include "cyber/quadrangulate/position_field.hpp"

using cyber::Index;
using cyber::Mesh;
using cyber::Vec3;
namespace remesh = cyber::remesh;

namespace {

// Open triangulated cylinder about +z. Its principal directions are the axis
// and the circumference, so a well-behaved 4-RoSy field aligns to them.
Mesh cylinder(float radius, float height, int rings, int segments) {
    std::vector<Vec3> p;
    std::vector<std::vector<Index>> f;
    const auto id = [&](int i, int j) { return static_cast<Index>(i * segments + (j % segments)); };
    for (int i = 0; i <= rings; ++i) {
        const float z = height * static_cast<float>(i) / static_cast<float>(rings);
        for (int j = 0; j < segments; ++j) {
            const float a = 2.0f * 3.14159265358979323846f * static_cast<float>(j) /
                            static_cast<float>(segments);
            p.push_back({radius * std::cos(a), radius * std::sin(a), z});
        }
    }
    for (int i = 0; i < rings; ++i) {
        for (int j = 0; j < segments; ++j) {
            f.push_back({id(i, j), id(i + 1, j), id(i + 1, j + 1)});
            f.push_back({id(i, j), id(i + 1, j + 1), id(i, j + 1)});
        }
    }
    return Mesh::fromIndexed(p, f);
}

// Closed UV sphere. Its two poles are 4-RoSy singularities, so the extractor's
// face walk produces n-gon orbits there — the regime that exercises the
// greedy-cut hole fill (fillFace), unlike the developable cylinder.
Mesh uvSphere(float radius, int rings, int segments) {
    std::vector<Vec3> p;
    std::vector<std::vector<Index>> f;
    const auto id = [&](int i, int j) { return static_cast<Index>(i * segments + (j % segments)); };
    for (int i = 0; i <= rings; ++i) {
        const float theta = 3.14159265358979323846f * static_cast<float>(i) / static_cast<float>(rings);
        for (int j = 0; j < segments; ++j) {
            const float phi = 2.0f * 3.14159265358979323846f * static_cast<float>(j) /
                              static_cast<float>(segments);
            p.push_back({radius * std::sin(theta) * std::cos(phi),
                         radius * std::sin(theta) * std::sin(phi), radius * std::cos(theta)});
        }
    }
    for (int i = 0; i < rings; ++i) {
        for (int j = 0; j < segments; ++j) {
            f.push_back({id(i, j), id(i + 1, j), id(i + 1, j + 1)});
            f.push_back({id(i, j), id(i + 1, j + 1), id(i, j + 1)});
        }
    }
    return Mesh::fromIndexed(p, f);
}

// Subdivided icosahedron: a near-uniform triangulation of the sphere (unlike a
// UV sphere it has no pole clustering), so its collapsed lattice graph reaches a
// clean, high valence — the right probe for the edge-recovery pass.
Mesh icosphere(int subdivisions) {
    const float t = (1.0f + std::sqrt(5.0f)) * 0.5f;
    std::vector<Vec3> p = {{-1, t, 0}, {1, t, 0}, {-1, -t, 0}, {1, -t, 0}, {0, -1, t}, {0, 1, t},
                           {0, -1, -t}, {0, 1, -t}, {t, 0, -1}, {t, 0, 1}, {-t, 0, -1}, {-t, 0, 1}};
    std::vector<std::array<int, 3>> tri = {
        {0, 11, 5}, {0, 5, 1}, {0, 1, 7}, {0, 7, 10}, {0, 10, 11}, {1, 5, 9}, {5, 11, 4},
        {11, 10, 2}, {10, 7, 6}, {7, 1, 8}, {3, 9, 4}, {3, 4, 2}, {3, 2, 6}, {3, 6, 8},
        {3, 8, 9}, {4, 9, 5}, {2, 4, 11}, {6, 2, 10}, {8, 6, 7}, {9, 8, 1}};
    for (int s = 0; s < subdivisions; ++s) {
        std::vector<std::array<int, 3>> nt;
        std::map<std::pair<int, int>, int> mid;
        const auto mp = [&](int a, int b) {
            const auto k = std::minmax(a, b);
            const auto it = mid.find({k.first, k.second});
            if (it != mid.end()) {
                return it->second;
            }
            const int idx = static_cast<int>(p.size());
            p.push_back((p[static_cast<std::size_t>(a)] + p[static_cast<std::size_t>(b)]) * 0.5f);
            mid[{k.first, k.second}] = idx;
            return idx;
        };
        for (const auto& f : tri) {
            const int a = mp(f[0], f[1]), b = mp(f[1], f[2]), c = mp(f[2], f[0]);
            nt.push_back({f[0], a, c});
            nt.push_back({f[1], b, a});
            nt.push_back({f[2], c, b});
            nt.push_back({a, b, c});
        }
        tri = std::move(nt);
    }
    std::vector<Vec3> pn;
    std::vector<std::vector<Index>> fn;
    for (const auto& v : p) {
        pn.push_back(normalized(v));
    }
    for (const auto& f : tri) {
        fn.push_back({static_cast<Index>(f[0]), static_cast<Index>(f[1]), static_cast<Index>(f[2])});
    }
    return Mesh::fromIndexed(pn, fn);
}

// Mean edge length of a mesh (used to size the field to the input).
float meanEdgeLength(const Mesh& m) {
    std::vector<Vec3> P;
    std::vector<std::vector<Index>> F;
    m.toIndexed(P, F);
    double sum = 0.0;
    std::size_t n = 0;
    for (const auto& f : F) {
        for (std::size_t k = 0; k < f.size(); ++k) {
            sum += static_cast<double>(length(P[f[k]] - P[f[(k + 1) % f.size()]]));
            ++n;
        }
    }
    return n ? static_cast<float>(sum / static_cast<double>(n)) : 1.0f;
}

// A flat quad grid whose columns get progressively wider, so local edge length
// grades from fine (x=0 side) to coarse (far side). Exercises the position
// field's per-vertex density scale (adaptive/variable-spacing support).
Mesh gradedGrid(int nx, int ny) {
    std::vector<float> xs(static_cast<std::size_t>(nx) + 1);
    float x = 0.0f;
    for (int j = 0; j <= nx; ++j) {
        xs[static_cast<std::size_t>(j)] = x;
        x += 0.05f + 0.20f * static_cast<float>(j) / static_cast<float>(nx);  // widening columns
    }
    std::vector<Vec3> p;
    for (int i = 0; i <= ny; ++i) {
        for (int j = 0; j <= nx; ++j) {
            p.push_back({xs[static_cast<std::size_t>(j)], 0.25f * static_cast<float>(i), 0.0f});
        }
    }
    const auto id = [&](int i, int j) { return static_cast<Index>(i * (nx + 1) + j); };
    std::vector<std::vector<Index>> f;
    for (int i = 0; i < ny; ++i) {
        for (int j = 0; j < nx; ++j) {
            f.push_back({id(i, j), id(i, j + 1), id(i + 1, j + 1), id(i + 1, j)});
        }
    }
    return Mesh::fromIndexed(p, f);
}

// Best 4-RoSy agreement between two tangent-plane directions about normal n.
float rosyAgreement(Vec3 a, Vec3 b, Vec3 n) {
    float best = -1.0f;
    for (int k = 0; k < 4; ++k) {
        best = std::fmax(best, dot(a, b));
        b = cross(n, b);  // rotate 90 degrees
    }
    return best;
}

}  // namespace

// The per-vertex 4-RoSy orientation field must be smooth (neighbouring crosses
// agree under 90-degree symmetry) and, on a cylinder, aligned to the principal
// directions (axis / circumference) — i.e. every vertex's cross is essentially
// axial or circumferential, never diagonal.
TEST_CASE("position field: orientation is smooth and principal-direction aligned") {
    const Mesh cyl = cylinder(1.0f, 3.0f, 40, 40);
    const remesh::PositionField field = remesh::computePositionField(cyl, 0.2f, 25);

    std::vector<Vec3> P;
    std::vector<std::vector<Index>> F;
    cyl.toIndexed(P, F);

    double agreeSum = 0.0;
    std::size_t edgeCount = 0;
    double alignSum = 0.0;
    std::size_t vertCount = 0;
    for (const auto& tri : F) {
        for (int k = 0; k < 3; ++k) {
            const std::size_t i = tri[static_cast<std::size_t>(k)];
            const std::size_t j = tri[static_cast<std::size_t>((k + 1) % 3)];
            if (!field.valid[i] || !field.valid[j]) {
                continue;
            }
            agreeSum += static_cast<double>(rosyAgreement(field.q[i], field.q[j], field.normal[i]));
            ++edgeCount;
        }
    }
    for (std::size_t i = 0; i < field.size(); ++i) {
        if (!field.valid[i]) {
            continue;
        }
        // Axis alignment: the cross's closer axis to the z axis should be nearly
        // parallel or nearly perpendicular, never at ~45 degrees. Measure how far
        // the field direction is from a principal axis: min over the 4 RoSy reps
        // of the angle to the z axis, folded into [0, 45] degrees; expect small.
        const Vec3 q = field.q[i];
        const Vec3 qp = field.qPerp(i);
        const float axial = std::fmax(std::fabs(q.z), std::fabs(qp.z));  // 1 = perfectly aligned
        alignSum += static_cast<double>(axial);
        ++vertCount;
    }

    REQUIRE(edgeCount > 0);
    REQUIRE(vertCount > 0);
    const double agreement = agreeSum / static_cast<double>(edgeCount);
    const double alignment = alignSum / static_cast<double>(vertCount);
    CAPTURE(agreement);
    CAPTURE(alignment);
    CHECK(agreement > 0.95);   // neighbouring crosses nearly identical under RoSy
    CHECK(alignment > 0.9);    // one field axis runs along the cylinder axis
}

// The wired position-field quadrangulator must turn a closed triangle mesh at
// ~target edge length into a valid, strongly quad-dominant mesh — the
// rotation-system walk + fan-split cover the whole surface (only singularity
// triangles remain), unlike the old ~30%-coverage walk.
TEST_CASE("instant-meshes quadrangulator gives a valid quad-dominant mesh") {
    Mesh cyl = cylinder(1.0f, 3.0f, 12, 25);  // ~unit edge length grid
    cyl.tagFeatureEdges(90.0f);
    auto quad = remesh::makeInstantMeshesQuadrangulator(25);
    const auto outcome = quad->quadrangulate(cyl, 0.25f, nullptr, nullptr);
    REQUIRE(outcome.success);
    REQUIRE(quad->name() == "instant-meshes");
    REQUIRE(cyl.validate().empty());

    std::vector<Vec3> P;
    std::vector<std::vector<Index>> F;
    cyl.toIndexed(P, F);
    std::size_t quads = 0;
    for (const auto& f : F) {
        if (f.size() == 4) {
            ++quads;
        }
    }
    CAPTURE(F.size());
    CAPTURE(quads);
    REQUIRE(F.size() > 0);
    CHECK(static_cast<double>(quads) / static_cast<double>(F.size()) > 0.9);
}

// The position field must stay on the surface near each vertex (it is a
// representative of that vertex's lattice cell, not a free point): every o_i is
// within a small multiple of the spacing of its vertex.
TEST_CASE("position field: representatives stay near their vertices") {
    const Mesh cyl = cylinder(1.0f, 3.0f, 30, 30);
    const float spacing = 0.2f;
    const remesh::PositionField field = remesh::computePositionField(cyl, spacing, 20);

    std::vector<Vec3> P;
    std::vector<std::vector<Index>> F;
    cyl.toIndexed(P, F);
    REQUIRE(P.size() == field.size());

    float maxDrift = 0.0f;
    for (std::size_t i = 0; i < field.size(); ++i) {
        if (field.valid[i]) {
            maxDrift = std::fmax(maxDrift, cyber::length(field.o[i] - P[i]));
        }
    }
    CAPTURE(maxDrift);
    CHECK(maxDrift < spacing);  // representative stays within a cell of its vertex
}

// Full extraction on a developable surface (a cylinder, no singularities) with
// the mesh already at ~target edge length — the regime the pipeline's isotropic
// stage produces. The position-field extraction must yield a valid, fully
// quad-dominant grid. (Surfaces with singularities need the Stage-C handling
// and are exercised separately once that lands.)
TEST_CASE("position field: extraction gives a clean quad grid on a cylinder") {
    const float spacing = 0.25f;
    const int rings = 12;     // height 3 / spacing
    const int segments = 25;  // circumference 2*pi / spacing
    const Mesh cyl = cylinder(1.0f, 3.0f, rings, segments);
    const remesh::PositionField field = remesh::computePositionField(cyl, spacing, 60);
    const Mesh quads = remesh::extractQuadMesh(cyl, field);

    std::vector<Vec3> P;
    std::vector<std::vector<Index>> F;
    quads.toIndexed(P, F);
    std::size_t quadCount = 0;
    for (const auto& f : F) {
        if (f.size() == 4) {
            ++quadCount;
        }
    }
    CAPTURE(F.size());
    CAPTURE(quadCount);
    REQUIRE(F.size() > 0);
    CHECK(quads.validate().empty());                                             // manifold
    CHECK(static_cast<double>(quadCount) / static_cast<double>(F.size()) > 0.90);  // quad grid
}

// The position field's per-vertex lattice-spacing scale must track LOCAL mesh
// density: on a graded grid (fine columns → coarse columns) the fine side gets a
// sub-1 multiplier and the coarse side a super-1 one. This is what lets the
// extractor follow adaptive sizing instead of over-merging the coarse regions; a
// uniform mesh keeps scale ~1 everywhere (so the fixed-spacing path is unchanged).
TEST_CASE("position field: per-vertex scale tracks local density") {
    const Mesh grid = gradedGrid(22, 8);
    const float spacing = meanEdgeLength(grid);
    const remesh::PositionField field = remesh::computePositionField(grid, spacing, 20);

    float minScale = 1e9f, maxScale = 0.0f;
    for (std::size_t i = 0; i < field.size(); ++i) {
        if (field.valid[i]) {
            minScale = std::fmin(minScale, field.scale[i]);
            maxScale = std::fmax(maxScale, field.scale[i]);
        }
    }
    CAPTURE(minScale);
    CAPTURE(maxScale);
    CHECK(minScale < 0.85f);  // fine columns: sub-mean spacing (heavy smoothing caps the range)
    CHECK(maxScale > 1.15f);  // coarse columns: super-mean spacing

    // A uniform mesh must stay ~1 everywhere (fixed-spacing path unchanged).
    const Mesh cyl = cylinder(1.0f, 3.0f, 30, 30);
    const remesh::PositionField uni = remesh::computePositionField(cyl, 0.2f, 20);
    float uMin = 1e9f, uMax = 0.0f;
    for (std::size_t i = 0; i < uni.size(); ++i) {
        if (uni.valid[i]) {
            uMin = std::fmin(uMin, uni.scale[i]);
            uMax = std::fmax(uMax, uni.scale[i]);
        }
    }
    CAPTURE(uMin);
    CAPTURE(uMax);
    CHECK(uMin > 0.8f);   // uniform: scale stays near 1
    CHECK(uMax < 1.25f);
}

// Integer-parametrization rewrite, Milestone 2: the min-cost max-flow engine
// that will balance per-face coordinate divergences in the integer solve must
// reproduce known-answer flow problems (cheapest-paths routing).
TEST_CASE("integer solve: min-cost max-flow engine is correct") {
    CHECK(remesh::debugMinCostFlow());
}

// Milestone 2 — the integer solve. On a closed genus-0 icosphere the min-cost
// flow must cancel most of the position-field's spurious divergences: the residual
// recomputed from the edge diffs / orientations matches the position singularities
// (checkpoint A: residualMismatch ~0), and the post-solve singularity count is a
// small fraction of the pre-solve one (the flow removed the spurious defects,
// leaving only the topologically required singularities).
TEST_CASE("integer solve: min-cost flow cancels spurious singularities") {
    const Mesh sphere = icosphere(3);
    const float spacing = meanEdgeLength(sphere);
    const remesh::PositionField field = remesh::computePositionField(sphere, spacing, 40);
    const remesh::IntegerSolveStats s = remesh::debugIntegerSolve(sphere, field);
    CAPTURE(s.faces);
    CAPTURE(s.preSingular);
    CAPTURE(s.residualMismatch);
    CAPTURE(s.postSingular);
    REQUIRE(s.preSingular > 0);
    CHECK(s.residualMismatch < s.faces / 50);          // setup consistent (checkpoint A)
    CHECK(s.postSingular < s.preSingular * 2 / 5);      // flow removed most spurious singularities
}

// Milestone 3, stage (a): after subdivision every triangle edge spans at most one
// grid cell (|edge_diff component| <= 1) — the invariant the collapse + quad
// extraction relies on. The subdivided mesh must be a strict refinement (no fewer
// triangles) and stay finite (no runaway split).
TEST_CASE("integer solve: subdivision makes every edge span at most one cell") {
    const Mesh sphere = icosphere(3);
    const float spacing = meanEdgeLength(sphere);
    const remesh::PositionField field = remesh::computePositionField(sphere, spacing, 40);
    const remesh::SubdivideStats s = remesh::debugSubdivide(sphere, field);
    CAPTURE(s.trisBefore);
    CAPTURE(s.trisAfter);
    CAPTURE(s.vertsAfter);
    CAPTURE(s.maxDiff);
    REQUIRE(s.trisBefore > 0);
    CHECK(s.maxDiff <= 1);                        // the extraction invariant
    CHECK(s.trisAfter >= s.trisBefore);           // refinement only
    CHECK(s.trisAfter < s.trisBefore * 4);        // no runaway (few edges span >1 cell)
}

// Milestone 5a — the flip-repair layer's coarsen+propagate round trip. On a clean
// developable cylinder grid, DownsampleEdgeGraph coarsens to a fixpoint and
// PropagateEdge lifts it back; with NO flip repair this MUST be an exact identity
// on edge_diff. This is the single highest-risk part of the port (the merge-path
// orientation transport), so it is locked cylinder-first before FixFlip is wired.
TEST_CASE("flip repair: coarsen+propagate is an identity on a clean cylinder grid") {
    const Mesh cyl = cylinder(1.0f, 3.0f, 40, 40);
    const remesh::PositionField field = remesh::computePositionField(cyl, 0.16f, 40);
    const remesh::FlipRepairStats s = remesh::debugFlipRepair(cyl, field);
    CAPTURE(s.faces);
    CAPTURE(s.levels);
    CAPTURE(s.roundTripMismatch);
    CAPTURE(s.flippedBefore);
    CAPTURE(s.flippedAfter);
    REQUIRE(s.faces > 0);
    CHECK(s.levels > 1);                            // coarsening actually happened
    CHECK(s.roundTripMismatch == 0);                // exact round trip (transport correct)
    CHECK(s.residualAfter == s.residualBefore);     // integrability preserved
    CHECK(s.flippedAfter <= s.flippedBefore);       // never introduces folds
}

// Milestone 5a — the round trip is an identity on the closed icosphere too (a
// curved grid with genuine singularities, unlike the developable cylinder).
TEST_CASE("flip repair: coarsen+propagate is an identity on an icosphere grid") {
    const Mesh sphere = icosphere(3);
    const remesh::PositionField field = remesh::computePositionField(sphere, meanEdgeLength(sphere), 40);
    const remesh::FlipRepairStats s = remesh::debugFlipRepair(sphere, field);
    CAPTURE(s.levels);
    CAPTURE(s.roundTripMismatch);
    REQUIRE(s.faces > 0);
    CHECK(s.levels > 1);
    CHECK(s.roundTripMismatch == 0);
    CHECK(s.residualAfter == s.residualBefore);
}

// Milestones 3–4 — extraction via QuadriFlow's BuildTriangleManifold + FixValence
// (single-level): reconstruct a clean compact triangle manifold (zero-diff
// collapse + orbit-walk vertex split), pair the two triangles across each
// grid-cell diagonal into a quad, FixHoles the residual boundary loops, then
// dissolve interior valence-2 doublets (FixValence). The output is all-quad, a
// valid manifold, and near-watertight: the naive collapse-and-pair left ~312
// boundary edges / ~45% irregular; the manifold reconstruction cuts that to a few
// dozen boundary edges (only unfillable triangular holes at singularities remain),
// and the doublet pass trims the irregular count further.
TEST_CASE("integer solve: extraction is manifold, all-quad and near-watertight") {
    const Mesh sphere = icosphere(3);
    const float spacing = meanEdgeLength(sphere);
    const remesh::PositionField field = remesh::computePositionField(sphere, spacing, 40);

    const Mesh quads = remesh::extractIntegerQuadMesh(sphere, field);
    CHECK(quads.validate().empty());  // a valid manifold (was non-manifold before)

    const remesh::IntegerExtractStats s = remesh::debugIntegerExtract(sphere, field);
    CAPTURE(s.quads);
    CAPTURE(s.verts);
    CAPTURE(s.irregular);
    CAPTURE(s.boundaryEdges);
    REQUIRE(s.quads > 0);
    CHECK(s.nonQuad == 0);                  // every emitted face is a quad
    CHECK(s.boundaryEdges < s.quads / 10);  // near-watertight (was > 60%)
    CHECK(s.irregular < s.verts / 4);       // mostly valence-4 (was ~45%)
}

// Milestone 2 foundation: QuadriFlow's per-face joint-alignment residual. On a
// developable cylinder the position field is a near-clean grid, so almost no
// face is a position singularity — the input the Stage-2 flow must drive to
// zero. (Curved corpus meshes have ~11% pre-solve; the flow reduces that.)
TEST_CASE("integer solve: cylinder has few position singularities") {
    const Mesh cyl = cylinder(1.0f, 3.0f, 40, 40);
    const remesh::PositionField field = remesh::computePositionField(cyl, 0.16f, 40);
    std::size_t faces = 0;
    for (cyber::Index fi = 0; fi < cyl.faceCapacity(); ++fi) {
        if (cyl.isAlive(cyber::FaceId{fi}) && cyl.faceSize(cyber::FaceId{fi}) == 3) {
            ++faces;
        }
    }
    const std::size_t sing = remesh::debugPositionSingularities(cyl, field);
    CAPTURE(faces);
    CAPTURE(sing);
    REQUIRE(faces > 0);
    CHECK(static_cast<double>(sing) / static_cast<double>(faces) < 0.03);  // near-clean grid
}

// Integer-parametrization rewrite, Milestone 1: the per-face holonomy of the
// field connection must find NO orientation singularities on a developable
// cylinder (its 4-RoSy field is clean). This locks in the connection rotation
// math (rotation index = difference of the aligned representative indices) that
// the Stage-2 integer solve will build on. Translation defects may be nonzero
// (position-field boundary noise) — that is what the integer solve resolves.
TEST_CASE("integer consistency: orientation field is singularity-clean on a cylinder") {
    const Mesh cyl = cylinder(1.0f, 3.0f, 40, 40);
    const remesh::PositionField field = remesh::computePositionField(cyl, 0.16f, 40);
    const remesh::IntegerConsistency ic = remesh::measureIntegerConsistency(cyl, field);
    CAPTURE(ic.loopEdges);  // faces tested
    CAPTURE(ic.rotSingular);
    REQUIRE(ic.loopEdges > 0);
    CHECK(ic.rotSingular == 0);  // developable surface: zero orientation holonomy
}

// Regression for the post-collapse lattice-edge recovery (A4b). Per-vertex
// field noise misclassifies some real lattice edges as diagonals, dropping the
// collapsed graph's average valence below the grid ideal of 4. Recovery re-tests
// cross-node mesh edges by the cleaner node-centroid distance and restores them.
// Without recovery a UV sphere sits near valence 3.75; with it, near the ideal.
TEST_CASE("position field: collapse graph reaches near-grid valence") {
    const Mesh sphere = icosphere(3);
    const float spacing = meanEdgeLength(sphere);
    const remesh::PositionField field = remesh::computePositionField(sphere, spacing, 60);
    const remesh::CollapsedGraphStats st = remesh::debugCollapse(sphere, field);
    REQUIRE(st.nodes > 0);
    const double valence = 2.0 * static_cast<double>(st.latticeEdges) / static_cast<double>(st.nodes);
    CAPTURE(st.nodes);
    CAPTURE(st.latticeEdges);
    CAPTURE(valence);
    CHECK(valence > 3.85);  // recovery lifts it from ~3.74 toward the grid ideal 4
}

// Regression for the greedy-cut hole fill (fillFace). A UV sphere's poles are
// singularities, so extraction produces n-gon orbits there. The old centroid
// fan-split filled them with pie-slice quads (many corners well under 30 deg);
// greedy-cut instead emits near-90-deg quads from the orbit's own vertices. The
// result must stay manifold and quad-dominant, and only a small fraction of
// quads may have a sharp (< 30 deg) corner — a centroid fan would blow past this.
TEST_CASE("position field: greedy-cut hole fill avoids pie-slice slivers") {
    const Mesh sphere = uvSphere(1.0f, 24, 30);
    const remesh::PositionField field = remesh::computePositionField(sphere, 0.14f, 40);
    const Mesh quads = remesh::extractQuadMesh(sphere, field);

    std::vector<Vec3> P;
    std::vector<std::vector<Index>> F;
    quads.toIndexed(P, F);
    REQUIRE(F.size() > 0);
    CHECK(quads.validate().empty());  // manifold

    std::size_t quadCount = 0;
    std::size_t slivers = 0;  // quads with a corner < 30 deg
    for (const auto& f : F) {
        if (f.size() != 4) {
            continue;
        }
        ++quadCount;
        float worst = 180.0f;
        for (int k = 0; k < 4; ++k) {
            const Vec3 a = P[f[static_cast<std::size_t>(k)]];
            const Vec3 b = P[f[static_cast<std::size_t>((k + 1) % 4)]];
            const Vec3 pr = P[f[static_cast<std::size_t>((k + 3) % 4)]];
            const Vec3 e1 = b - a, e2 = pr - a;
            const float c = dot(e1, e2) / (length(e1) * length(e2) + 1e-6f);
            worst = std::fmin(worst, std::acos(std::clamp(c, -1.0f, 1.0f)) * 180.0f / 3.14159265f);
        }
        if (worst < 30.0f) {
            ++slivers;
        }
    }
    REQUIRE(quadCount > 0);
    const double quadFrac = static_cast<double>(quadCount) / static_cast<double>(F.size());
    const double sliverFrac = static_cast<double>(slivers) / static_cast<double>(quadCount);
    CAPTURE(F.size());
    CAPTURE(quadFrac);
    CAPTURE(sliverFrac);
    CHECK(quadFrac > 0.75);       // strongly quad-dominant despite the two poles
    CHECK(sliverFrac < 0.10);     // greedy-cut keeps pie-slice slivers rare
}
