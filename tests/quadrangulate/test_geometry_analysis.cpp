#include <doctest.h>

#include <cmath>
#include <cstddef>
#include <vector>

#include "cyber/core/bvh.hpp"
#include "cyber/core/mesh.hpp"
#include "cyber/quadrangulate/geometry_analysis.hpp"
#include "cyber/quadrangulate/layout_score.hpp"

using cyber::Bvh;
using cyber::FaceId;
using cyber::Index;
using cyber::Mesh;
using cyber::Vec3;
using cyber::VertexId;
using cyber::remesh::analyzeGeometry;
using cyber::remesh::GeometryAnalysis;
using cyber::remesh::LayoutNode;
using cyber::remesh::LayoutNodeKind;
using cyber::remesh::scoreSingularities;
using cyber::remesh::singularityCost;
using cyber::remesh::SingularityWeights;
using cyber::remesh::TopologyLayout;

namespace {

// A flat n x n grid of quads in the z = 0 plane.
Mesh flatGrid(int n, float step = 1.0f) {
    std::vector<Vec3> p;
    for (int y = 0; y <= n; ++y) {
        for (int x = 0; x <= n; ++x) {
            p.push_back(Vec3{static_cast<float>(x) * step, static_cast<float>(y) * step, 0.0f});
        }
    }
    const auto at = [n](int x, int y) { return static_cast<Index>(y * (n + 1) + x); };
    std::vector<std::vector<Index>> f;
    for (int y = 0; y < n; ++y) {
        for (int x = 0; x < n; ++x) {
            f.push_back({at(x, y), at(x + 1, y), at(x + 1, y + 1), at(x, y + 1)});
        }
    }
    return Mesh::fromIndexed(p, f);
}

// The two faces of a slab of thickness `gap`. Each sheet's OUTWARD normal
// points away from the material between them, so the inward probe (-n) crosses
// the gap and hits the opposite sheet — which is exactly the configuration the
// thickness estimate is defined for.
Mesh parallelSheets(float gap) {
    std::vector<Vec3> p = {
        {0, 0, 0},   {4, 0, 0},   {4, 4, 0},   {0, 4, 0},    // lower face of the slab
        {0, 0, gap}, {4, 0, gap}, {4, 4, gap}, {0, 4, gap},  // upper face
    };
    std::vector<std::vector<Index>> f = {
        {0, 3, 2, 1},  // wound so its normal points DOWN, out of the slab
        {4, 5, 6, 7},  // wound so its normal points UP, out of the slab
    };
    return Mesh::fromIndexed(p, f);
}

std::size_t aliveVertices(const Mesh& mesh) {
    std::size_t n = 0;
    for (Index v = 0; v < mesh.vertexCapacity(); ++v) {
        n += mesh.isAlive(VertexId{v}) ? 1u : 0u;
    }
    return n;
}

}  // namespace

TEST_CASE("geometry analysis: a flat sheet has no curvature anywhere") {
    const Mesh mesh = flatGrid(4);
    const GeometryAnalysis g = analyzeGeometry(mesh, 1.0f);
    REQUIRE(g.valid);
    REQUIRE(g.normalizedCurvature.size() == mesh.vertexCapacity());
    for (Index v = 0; v < mesh.vertexCapacity(); ++v) {
        if (mesh.isAlive(VertexId{v})) {
            CHECK(g.normalizedCurvature[v] == doctest::Approx(0.0).epsilon(1e-5));
        }
    }
}

TEST_CASE("geometry analysis: a folded sheet reports curvature at the fold") {
    // Two quads meeting at 90 degrees: the shared edge's vertices see a
    // half-pi normal deviation, i.e. 0.5 once normalized by pi. The far
    // corners see only their own flat face.
    std::vector<Vec3> p = {{0, 0, 0}, {1, 0, 0}, {1, 1, 0}, {0, 1, 0}, {0, 0, 1}, {1, 0, 1}};
    std::vector<std::vector<Index>> f = {{0, 1, 2, 3}, {0, 4, 5, 1}};
    const Mesh mesh = Mesh::fromIndexed(p, f);
    const GeometryAnalysis g = analyzeGeometry(mesh, 1.0f);
    REQUIRE(g.valid);
    // Vertices 0 and 1 are on the fold; 2 and 3 are not.
    CHECK(g.normalizedCurvature[0] == doctest::Approx(0.5).epsilon(1e-3));
    CHECK(g.normalizedCurvature[1] == doctest::Approx(0.5).epsilon(1e-3));
    CHECK(g.normalizedCurvature[2] == doctest::Approx(0.0).epsilon(1e-5));
    CHECK(g.normalizedCurvature[3] == doctest::Approx(0.0).epsilon(1e-5));
}

TEST_CASE("geometry analysis: boundary influence is 1 on the border and decays inward") {
    const Mesh mesh = flatGrid(6);
    cyber::remesh::GeometryAnalysisOptions opts;
    opts.influenceRadius = 3.0f;  // three edge lengths
    const GeometryAnalysis g = analyzeGeometry(mesh, 1.0f, nullptr, opts);
    REQUIRE(g.valid);
    // Corner (0,0) is on the border; the grid centre (3,3) is three steps in,
    // which is exactly the radius, so its influence has decayed to zero.
    const Index corner = 0;
    const Index centre = static_cast<Index>(3 * 7 + 3);
    CHECK(g.boundaryInfluence[corner] == doctest::Approx(1.0));
    CHECK(g.boundaryInfluence[centre] == doctest::Approx(0.0));
    // ...and one step in from the border sits strictly between.
    const Index nearEdge = static_cast<Index>(1 * 7 + 1);
    CHECK(g.boundaryInfluence[nearEdge] > 0.0f);
    CHECK(g.boundaryInfluence[nearEdge] < 1.0f);
}

TEST_CASE("geometry analysis: a closed surface has no boundary influence") {
    // A tetrahedron: closed, so no edge is a boundary edge.
    std::vector<Vec3> p = {{0, 0, 0}, {1, 0, 0}, {0, 1, 0}, {0, 0, 1}};
    std::vector<std::vector<Index>> f = {{0, 2, 1}, {0, 1, 3}, {0, 3, 2}, {1, 2, 3}};
    const Mesh mesh = Mesh::fromIndexed(p, f);
    const GeometryAnalysis g = analyzeGeometry(mesh, 0.5f);
    REQUIRE(g.valid);
    for (Index v = 0; v < mesh.vertexCapacity(); ++v) {
        if (mesh.isAlive(VertexId{v})) {
            CHECK(g.boundaryInfluence[v] == doctest::Approx(0.0));
        }
    }
}

TEST_CASE("geometry analysis: the thickness probe measures a thin gap") {
    const float gap = 0.5f;
    const Mesh mesh = parallelSheets(gap);
    const Bvh bvh(mesh);
    REQUIRE_FALSE(bvh.empty());

    cyber::remesh::GeometryAnalysisOptions opts;
    opts.thinFeatureFactor = 4.0f;
    // Target edge 0.25 -> the "thin" threshold is 1.0, and the 0.5 gap is
    // comfortably inside it.
    const GeometryAnalysis g = analyzeGeometry(mesh, 0.25f, &bvh, opts);
    REQUIRE(g.valid);

    std::size_t measured = 0;
    for (Index v = 0; v < mesh.vertexCapacity(); ++v) {
        if (!mesh.isAlive(VertexId{v}) || !std::isfinite(g.thickness[v])) {
            continue;
        }
        ++measured;
        CHECK(g.thickness[v] == doctest::Approx(gap).epsilon(0.15));
        CHECK(g.thinFeatureRisk[v] > 0.0f);
    }
    CHECK(measured == aliveVertices(mesh));
}

TEST_CASE("geometry analysis: without a BVH the thickness field is simply unknown") {
    // Not zero — unknown. A zero thickness would read as "infinitely thin" and
    // make every cone look catastrophic.
    const Mesh mesh = parallelSheets(0.5f);
    const GeometryAnalysis g = analyzeGeometry(mesh, 0.25f, nullptr);
    REQUIRE(g.valid);
    for (Index v = 0; v < mesh.vertexCapacity(); ++v) {
        if (mesh.isAlive(VertexId{v})) {
            CHECK_FALSE(std::isfinite(g.thickness[v]));
            CHECK(g.thinFeatureRisk[v] == doctest::Approx(0.0));
        }
    }
}

TEST_CASE("geometry analysis is deterministic") {
    const Mesh mesh = flatGrid(5);
    const Bvh bvh(mesh);
    const GeometryAnalysis first = analyzeGeometry(mesh, 0.5f, &bvh);
    for (int i = 0; i < 4; ++i) {
        const GeometryAnalysis again = analyzeGeometry(mesh, 0.5f, &bvh);
        CHECK(again.normalizedCurvature == first.normalizedCurvature);
        CHECK(again.featureInfluence == first.featureInfluence);
        CHECK(again.boundaryInfluence == first.boundaryInfluence);
        CHECK(again.thinFeatureRisk == first.thinFeatureRisk);
    }
}

TEST_CASE("singularity cost scales with the index magnitude") {
    GeometryAnalysis g;
    g.valid = true;
    g.normalizedCurvature.assign(4, 0.0f);
    g.featureInfluence.assign(4, 0.0f);
    g.boundaryInfluence.assign(4, 0.0f);
    g.thinFeatureRisk.assign(4, 0.0f);

    const SingularityWeights w;
    const double one = singularityCost(VertexId{0}, 1, g, w);
    const double two = singularityCost(VertexId{0}, 2, g, w);
    CHECK(one > 0.0);
    CHECK(two == doctest::Approx(2.0 * one));
    // A negative index costs the same as its positive twin: both are equally
    // extraordinary.
    CHECK(singularityCost(VertexId{0}, -1, g, w) == doctest::Approx(one));
}

TEST_CASE("singularity cost rises with salience, and each term is separable") {
    GeometryAnalysis g;
    g.valid = true;
    g.normalizedCurvature.assign(5, 0.0f);
    g.featureInfluence.assign(5, 0.0f);
    g.boundaryInfluence.assign(5, 0.0f);
    g.thinFeatureRisk.assign(5, 0.0f);
    // Vertex 1 curved, 2 on a feature, 3 thin, 4 on a boundary.
    g.normalizedCurvature[1] = 1.0f;
    g.featureInfluence[2] = 1.0f;
    g.thinFeatureRisk[3] = 1.0f;
    g.boundaryInfluence[4] = 1.0f;

    const SingularityWeights w;
    const double flat = singularityCost(VertexId{0}, 1, g, w);
    CHECK(singularityCost(VertexId{1}, 1, g, w) == doctest::Approx(flat + w.curvature));
    CHECK(singularityCost(VertexId{2}, 1, g, w) == doctest::Approx(flat + w.featureProximity));
    CHECK(singularityCost(VertexId{3}, 1, g, w) == doctest::Approx(flat + w.thinFeature));
    CHECK(singularityCost(VertexId{4}, 1, g, w) == doctest::Approx(flat + w.boundary));
    // Every placed cone costs strictly more than none at all.
    CHECK(flat == doctest::Approx(w.count));
}

TEST_CASE("scoring a layout ranks cone PLACEMENT, not cone count") {
    GeometryAnalysis g;
    g.valid = true;
    g.normalizedCurvature.assign(2, 0.0f);
    g.featureInfluence.assign(2, 0.0f);
    g.boundaryInfluence.assign(2, 0.0f);
    g.thinFeatureRisk.assign(2, 0.0f);
    g.featureInfluence[1] = 1.0f;  // vertex 1 sits on a crease

    const auto layoutWithConeAt = [](Index vertex) {
        TopologyLayout layout;
        LayoutNode n;
        n.id = 0;
        n.kind = LayoutNodeKind::Singularity;
        n.singularityIndex = 1;
        n.vertex = VertexId{vertex};
        n.face = FaceId{0};
        layout.nodes.push_back(n);
        return layout;
    };

    const auto good = scoreSingularities(layoutWithConeAt(0), g);
    const auto bad = scoreSingularities(layoutWithConeAt(1), g);
    // Same count, very different quality — which is the whole point.
    CHECK(good.count == bad.count);
    CHECK(bad.weightedCost > good.weightedCost);
    CHECK(bad.featureInfluenceMax == doctest::Approx(1.0));
    CHECK(good.featureInfluenceMax == doctest::Approx(0.0));
    CHECK(good.totalIndex == bad.totalIndex);
}

TEST_CASE("scoring an empty layout is zero, not a division by zero") {
    const GeometryAnalysis g;
    const TopologyLayout empty;
    const auto m = scoreSingularities(empty, g);
    CHECK(m.count == 0);
    CHECK(m.weightedCost == doctest::Approx(0.0));
    CHECK(m.meanCost == doctest::Approx(0.0));
    CHECK(m.totalIndex == 0);
}
