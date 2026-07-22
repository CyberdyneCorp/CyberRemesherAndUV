#include <doctest.h>

#include <vector>

#include "cyber/core/mesh.hpp"
#include "cyber/quadrangulate/mcf_layout.hpp"
#include "cyber/quadrangulate/position_field.hpp"

// M3c of docs/mcf-integer-layout-plan.md: the SciPP max-flow solve. Built only when
// CYBER_WITH_SCIPP is enabled (cyber_quadrangulate then links scipp::scipp and defines
// CYBER_HAVE_SCIPP, so solveMcfFlow is functional rather than a stub).
using namespace cyber;

namespace {

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

// Clean axis-aligned unit lattice field on the grid.
remesh::PositionField gridField(const Mesh& mesh) {
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
    return field;
}

// Every non-singular face's oriented edge-diff sum must be zero for a seamless layout.
bool isSeamless(const remesh::McfEdgeInfo& info, const remesh::McfConstraints& con,
                const std::vector<remesh::Vec2i>& diff) {
    for (std::size_t f = 0; f < info.faceEdgeIds.size(); ++f) {
        if (info.posSingularities.count(static_cast<int>(f))) {
            continue;
        }
        remesh::Vec2i sum{0, 0};
        for (int j = 0; j < 3; ++j) {
            const int e = info.faceEdgeIds[f][static_cast<std::size_t>(j)];
            const int o = con.faceEdgeOrients[f][static_cast<std::size_t>(j)];
            remesh::Vec2i d = diff[static_cast<std::size_t>(e)];
            if (o & 1) {
                d = {-d.y, d.x};
            }
            if (o >= 2) {
                d = {-d.x, -d.y};
            }
            sum.x += d.x;
            sum.y += d.y;
        }
        if (sum.x != 0 || sum.y != 0) {
            return false;
        }
    }
    return true;
}

}  // namespace

TEST_CASE("MCF flow leaves an already-seamless grid untouched") {
    Mesh mesh = makeGrid(6);
    const remesh::PositionField field = gridField(mesh);
    const remesh::McfEdgeInfo info = buildMcfEdgeInfo(mesh, field);
    const remesh::McfConstraints con = buildMcfConstraints(mesh, field, info);
    const remesh::McfFlowSetup setup = buildMcfFlowSetup(mesh, info, con);

    const remesh::McfSolveResult res = solveMcfFlow(info, con, setup);
    REQUIRE(res.valid);
    CHECK(res.supply == 0);
    CHECK(res.fullFlow);
    REQUIRE(res.edgeDiff.size() == setup.edgeDiff.size());
    for (std::size_t e = 0; e < res.edgeDiff.size(); ++e) {
        CHECK((res.edgeDiff[e] == setup.edgeDiff[e]));
    }
    CHECK(isSeamless(info, con, res.edgeDiff));
}

TEST_CASE("MCF flow corrects a perturbed edge back to a seamless layout") {
    Mesh mesh = makeGrid(6);
    const remesh::PositionField field = gridField(mesh);
    const remesh::McfEdgeInfo info = buildMcfEdgeInfo(mesh, field);
    const remesh::McfConstraints con = buildMcfConstraints(mesh, field, info);
    remesh::McfFlowSetup setup = buildMcfFlowSetup(mesh, info, con);

    // Perturb one interior edge (referenced by two faces) to create a residual.
    int perturbed = -1;
    for (std::size_t e = 0; e < con.e2d.size(); ++e) {
        if (con.e2d[e].first != -1 && con.e2d[e].second != -1) {
            perturbed = static_cast<int>(e);
            break;
        }
    }
    REQUIRE(perturbed >= 0);
    setup.edgeDiff[static_cast<std::size_t>(perturbed)].x += 1;
    // The perturbed layout is no longer seamless.
    REQUIRE_FALSE(isSeamless(info, con, setup.edgeDiff));

    const remesh::McfSolveResult res = solveMcfFlow(info, con, setup);
    REQUIRE(res.valid);
    CHECK(res.supply > 0);
    CHECK(res.fullFlow);
    // The flow fully cancels the residual: the corrected layout is seamless again.
    CHECK(isSeamless(info, con, res.edgeDiff));
}
