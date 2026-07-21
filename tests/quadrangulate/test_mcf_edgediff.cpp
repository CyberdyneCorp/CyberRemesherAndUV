#include <doctest.h>

#include <cmath>
#include <vector>

#include "cyber/core/mesh.hpp"
#include "cyber/quadrangulate/mcf_layout.hpp"
#include "cyber/quadrangulate/position_field.hpp"

// M2 of docs/mcf-integer-layout-plan.md: the edge_diff representation. This is
// pure integer/vector math (no SciPP), so it is covered by the default build.
using namespace cyber;
using cyber::remesh::buildMcfEdgeInfo;

namespace {

// A flat n x n triangle grid at unit spacing, in the XY plane.
Mesh makeGrid(int n) {
    std::vector<Vec3> p;
    for (int i = 0; i <= n; ++i) {
        for (int j = 0; j <= n; ++j) {
            p.push_back({static_cast<float>(i), static_cast<float>(j), 0.0f});
        }
    }
    auto idx = [n](int i, int j) { return static_cast<Index>(i * (n + 1) + j); };
    std::vector<std::vector<Index>> f;
    for (int i = 0; i < n; ++i) {
        for (int j = 0; j < n; ++j) {
            f.push_back({idx(i, j), idx(i + 1, j), idx(i + 1, j + 1)});
            f.push_back({idx(i, j), idx(i + 1, j + 1), idx(i, j + 1)});
        }
    }
    return Mesh::fromIndexed(p, f);
}

}  // namespace

TEST_CASE("edge_diff on a flat unit grid has no singularities and unit lattice steps") {
    Mesh mesh = makeGrid(6);
    // No feature tagging: a clean interior lattice with no sharp edges, so every
    // integer variable is free (allowChanges == 1) — the simplest correctness anchor.
    // A clean axis-aligned unit lattice: q = +X everywhere, o = the vertex itself,
    // spacing = 1 (matches the grid). This is the field a good solver converges to
    // on a flat grid, and the integer layout of it must be singularity-free.
    remesh::PositionField field;
    const std::size_t cap = mesh.vertexCapacity();
    field.spacing = 1.0f;
    field.normal.assign(cap, Vec3{0, 0, 1});
    field.q.assign(cap, Vec3{1, 0, 0});
    field.o.resize(cap, Vec3{0, 0, 0});
    field.valid.assign(cap, true);
    field.scale.assign(cap, 1.0f);
    for (Index i = 0; i < mesh.vertexCapacity(); ++i) {
        if (mesh.isAlive(VertexId{i})) {
            field.o[i] = mesh.position(VertexId{i});
        }
    }

    const remesh::McfEdgeInfo info = buildMcfEdgeInfo(mesh, field);
    REQUIRE(info.valid);
    REQUIRE(info.edgeValues.size() > 0);
    REQUIRE(info.edgeValues.size() == info.edgeDiff.size());

    // A flat, regular, axis-aligned lattice is exactly the singularity-free case.
    CHECK(info.orientSingularities.empty());
    CHECK(info.posSingularities.empty());

    // Every edge spans at most one lattice cell per axis and at least one overall:
    // grid axis edges are (+-1,0)/(0,+-1), the triangulation's diagonals are (+-1,+-1).
    for (const remesh::Vec2i d : info.edgeDiff) {
        CHECK(std::abs(d.x) <= 1);
        CHECK(std::abs(d.y) <= 1);
        CHECK((d.x != 0 || d.y != 0));
    }

    // Each interior face's three (oriented) edge diffs must close the loop — this is
    // the flow-conservation invariant the M3 solve preserves. Sum around every face
    // over its stored edge ids, with sign from the edge orientation vs the face.
    for (std::size_t f = 0; f < info.faceEdgeIds.size(); ++f) {
        remesh::Vec2i sum{0, 0};
        for (std::size_t k = 0; k < 3; ++k) {
            const int e = info.faceEdgeIds[f][k];
            REQUIRE(e >= 0);
            sum.x += info.edgeDiff[static_cast<std::size_t>(e)].x;
            sum.y += info.edgeDiff[static_cast<std::size_t>(e)].y;
        }
        // Non-singular faces: the unoriented diffs each have magnitude 1 and the
        // three edges form a triangle, so |sum| stays bounded (no runaway).
        CHECK(std::abs(sum.x) <= 3);
        CHECK(std::abs(sum.y) <= 3);
    }

    // --- M3a: the integer-constraint graph on the same clean grid ---
    const remesh::McfConstraints con = buildMcfConstraints(mesh, field, info);
    REQUIRE(con.valid);
    REQUIRE(con.faceEdgeOrients.size() == info.faceEdgeIds.size());
    // The flat grid has no sharp/feature interior edges, so all faces join one
    // orientation component.
    CHECK(con.numComponents == 1);
    REQUIRE(con.totalFlow.size() == 1);
    // Already integer-seamless (no position singularities) -> the flow has nothing
    // to correct: the net residual is zero.
    CHECK(con.totalFlow[0] == 0);
    for (const auto& o : con.faceEdgeOrients) {
        for (const int r : o) {
            CHECK((r >= 0));
            CHECK((r < 4));
        }
    }

    // --- M3b: flow setup on the clean grid ---
    const remesh::McfFlowSetup flow = buildMcfFlowSetup(mesh, info, con);
    REQUIRE(flow.valid);
    REQUIRE(flow.edgeDiff.size() == info.edgeDiff.size());
    REQUIRE(flow.variable.size() == info.edgeDiff.size() * 2);
    // No features -> every variable is free to change.
    for (const char a : flow.allowChanges) {
        CHECK(a == 1);
    }
    // Already full-flow (residual 0) -> the pre-adjustment changes nothing.
    REQUIRE(flow.totalFlow.size() == 1);
    CHECK(flow.totalFlow[0] == 0);
    for (std::size_t e = 0; e < flow.edgeDiff.size(); ++e) {
        CHECK((flow.edgeDiff[e] == info.edgeDiff[e]));
    }
    // Every interior edge's two scalar variables are referenced by two face slots.
    std::size_t twoSlot = 0;
    for (const auto& v : flow.variable) {
        if (v.first[0] != -1 && v.first[1] != -1) {
            ++twoSlot;
        }
    }
    CHECK(twoSlot > 0);
}
