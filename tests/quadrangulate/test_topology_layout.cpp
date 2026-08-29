#include <doctest.h>

#include <cstddef>
#include <cstdint>
#include <limits>
#include <string>
#include <vector>

#include "../../src/quadrangulate/src/topology_layout_build.hpp"
#include "cyber/quadrangulate/topology_layout.hpp"

using cyber::FaceId;
using cyber::Vec3;
using cyber::remesh::applyQuantization;
using cyber::remesh::LayoutArc;
using cyber::remesh::LayoutArcKind;
using cyber::remesh::layoutFromTMesh;
using cyber::remesh::LayoutNode;
using cyber::remesh::LayoutNodeKind;
using cyber::remesh::LayoutPatch;
using cyber::remesh::layoutToJson;
using cyber::remesh::layoutToObj;
using cyber::remesh::TopologyLayout;
using cyber::remesh::validateTopologyLayout;

namespace {

// A single closed quad patch: four corners, four separatrix arcs, one patch
// with one arc per side. The smallest layout that satisfies every invariant.
TopologyLayout closedQuad() {
    TopologyLayout layout;
    for (int i = 0; i < 4; ++i) {
        LayoutNode n;
        n.id = static_cast<std::uint32_t>(i);
        n.kind = LayoutNodeKind::Singularity;
        n.singularityIndex = 1;
        n.position = Vec3{static_cast<float>(i), 0.0f, 0.0f};
        n.face = FaceId{0};
        layout.nodes.push_back(n);
    }
    const std::uint32_t ends[4][2] = {{0, 1}, {1, 2}, {2, 3}, {3, 0}};
    for (int i = 0; i < 4; ++i) {
        LayoutArc a;
        a.id = static_cast<std::uint32_t>(i);
        a.begin = ends[i][0];
        a.end = ends[i][1];
        a.desiredLength = 2.0;
        a.samples = {{layout.nodes[a.begin].position, FaceId{0}},
                     {layout.nodes[a.end].position, FaceId{0}}};
        layout.arcs.push_back(a);
    }
    LayoutPatch p;
    p.id = 0;
    p.sides = {{0}, {1}, {2}, {3}};
    layout.patches.push_back(p);
    return layout;
}

}  // namespace

TEST_CASE("layout: a closed quad patch satisfies every invariant") {
    const TopologyLayout layout = closedQuad();
    const auto v = validateTopologyLayout(layout, 1);
    CHECK(v.ok);
    CHECK(v.clean());
    CHECK(v.error.empty());
    CHECK(v.nonClosingPatches.empty());

    const auto st = layout.stats();
    CHECK(st.nodes == 4);
    CHECK(st.arcs == 4);
    CHECK(st.patches == 1);
    CHECK(st.singularities == 4);
    CHECK(st.totalIndex == 4);
    CHECK(st.nonQuadPatches == 0);
    CHECK(layout.patches[0].quadLike());
}

TEST_CASE("layout: a T-junction side is a valid patch boundary") {
    // Side 1 is split by a T-junction into two arcs, so the patch still has
    // four logical sides but five arcs.
    TopologyLayout layout = closedQuad();
    LayoutNode t;
    t.id = 4;
    t.kind = LayoutNodeKind::TJunction;
    t.position = Vec3{1.0f, 0.5f, 0.0f};
    t.face = FaceId{0};
    layout.nodes.push_back(t);

    layout.arcs[1].end = 4;
    layout.arcs[1].samples.back().position = t.position;
    LayoutArc tail;
    tail.id = 4;
    tail.begin = 4;
    tail.end = 2;
    tail.desiredLength = 1.0;
    tail.samples = {{t.position, FaceId{0}}, {layout.nodes[2].position, FaceId{0}}};
    layout.arcs.push_back(tail);
    layout.patches[0].sides[1] = {1, 4};

    const auto v = validateTopologyLayout(layout, 1);
    CHECK(v.ok);
    CHECK(v.clean());
    CHECK(layout.stats().tJunctions == 1);
}

TEST_CASE("layout: a broken boundary walk is contained, not fatal") {
    TopologyLayout layout = closedQuad();
    // Detach the last side so the walk cannot return to its start.
    layout.arcs[3].begin = 1;
    layout.arcs[3].samples.front().position = layout.nodes[1].position;

    const auto v = validateTopologyLayout(layout, 1);
    CHECK(v.ok);  // the graph itself is still sound
    CHECK_FALSE(v.clean());
    REQUIRE(v.nonClosingPatches.size() == 1);
    CHECK(v.nonClosingPatches[0] == 0);
}

TEST_CASE("layout: hard structural violations are rejected with a named reason") {
    SUBCASE("arc endpoint is not a node") {
        TopologyLayout layout = closedQuad();
        layout.arcs[2].end = 99;
        const auto v = validateTopologyLayout(layout, 1);
        CHECK_FALSE(v.ok);
        CHECK(v.error.find("arc endpoint is not a node") != std::string::npos);
    }
    SUBCASE("non-finite node position") {
        TopologyLayout layout = closedQuad();
        layout.nodes[1].position.y = std::numeric_limits<float>::quiet_NaN();
        const auto v = validateTopologyLayout(layout, 1);
        CHECK_FALSE(v.ok);
        CHECK(v.error.find("non-finite node position") != std::string::npos);
    }
    SUBCASE("sample face outside the source mesh") {
        TopologyLayout layout = closedQuad();
        layout.arcs[0].samples[1].face = FaceId{7};
        const auto v = validateTopologyLayout(layout, 1);
        CHECK_FALSE(v.ok);
        CHECK(v.error.find("arc sample face out of range") != std::string::npos);
    }
    SUBCASE("singularity without an index") {
        TopologyLayout layout = closedQuad();
        layout.nodes[0].singularityIndex = 0;
        const auto v = validateTopologyLayout(layout, 1);
        CHECK_FALSE(v.ok);
        CHECK(v.error.find("singularity node with zero index") != std::string::npos);
    }
    SUBCASE("arc does not start at its begin node") {
        TopologyLayout layout = closedQuad();
        layout.arcs[0].samples.front().position = Vec3{9.0f, 9.0f, 9.0f};
        const auto v = validateTopologyLayout(layout, 1);
        CHECK_FALSE(v.ok);
        CHECK(v.error.find("arc does not start at its begin node") != std::string::npos);
    }
    SUBCASE("an arc bounding no patch must be marked excluded") {
        TopologyLayout layout = closedQuad();
        LayoutArc stray;
        stray.id = 4;
        stray.begin = 0;
        stray.end = 2;
        stray.desiredLength = 1.0;
        layout.arcs.push_back(stray);
        const auto v = validateTopologyLayout(layout, 1);
        CHECK_FALSE(v.ok);
        CHECK(v.error.find("arc bounds no patch") != std::string::npos);

        layout.arcs[4].excluded = true;
        CHECK(validateTopologyLayout(layout, 1).clean());
    }
}

TEST_CASE("layout: validation is deterministic") {
    TopologyLayout layout = closedQuad();
    layout.nodes[2].id = 5;  // a hard violation reachable after several checks
    const auto first = validateTopologyLayout(layout, 1);
    for (int i = 0; i < 8; ++i) {
        const auto again = validateTopologyLayout(layout, 1);
        CHECK(again.ok == first.ok);
        CHECK(again.error == first.error);
        CHECK(again.nonClosingPatches == first.nonClosingPatches);
    }
}

TEST_CASE("layout: exports are stable and reproducible") {
    const TopologyLayout layout = closedQuad();
    const std::string json = layoutToJson(layout);
    CHECK(json == layoutToJson(layout));
    CHECK(json.find("\"nodes\":4") != std::string::npos);
    CHECK(json.find("\"singularities\":4") != std::string::npos);
    CHECK(json.find("\"kind\":\"separatrix\"") != std::string::npos);

    const std::string obj = layoutToObj(layout);
    CHECK(obj == layoutToObj(layout));
    // One vertex per node, and one polyline per arc.
    std::size_t verts = 0, lines = 0;
    for (std::size_t i = 0; i < obj.size(); ++i) {
        if (i != 0 && obj[i - 1] != '\n') {
            continue;
        }
        if (obj.compare(i, 2, "v ") == 0) {
            ++verts;
        } else if (obj.compare(i, 2, "l ") == 0) {
            ++lines;
        }
    }
    CHECK(verts == 4);
    CHECK(lines == 4);
}

TEST_CASE("layout: promoting a T-mesh preserves its graph") {
    // The textbook cube T-mesh, as the tracer produces it for box_sharp:
    // 8 corner nodes, 12 arcs, 6 quad patches.
    cyber::remesh::bimdf::TMesh tm;
    tm.ok = true;
    tm.nodeCount = 8;
    const auto addArc = [&](std::size_t a, std::size_t b, double len, bool feature) {
        cyber::remesh::bimdf::Arc arc;
        arc.n0 = a;
        arc.n1 = b;
        arc.len = len;
        arc.onFeature = feature;
        tm.arcs.push_back(arc);
    };
    const std::size_t ends[12][2] = {{0, 1}, {2, 3}, {4, 5}, {6, 7}, {0, 2}, {1, 3},
                                     {4, 6}, {5, 7}, {0, 4}, {1, 5}, {2, 6}, {3, 7}};
    for (const auto& e : ends) {
        addArc(e[0], e[1], 3.0, true);
    }
    const std::size_t quads[6][4] = {{0, 5, 1, 4}, {2, 7, 3, 6},  {0, 9, 2, 8},
                                     {1, 11, 3, 10}, {4, 10, 6, 8}, {5, 11, 7, 9}};
    for (const auto& q : quads) {
        cyber::remesh::bimdf::Patch p;
        for (int k = 0; k < 4; ++k) {
            p.side[static_cast<std::size_t>(k)] = {q[k]};
        }
        tm.patches.push_back(p);
    }
    // Geometry: the unit cube's corners, one compact face mapping to itself.
    tm.nodeGeom.resize(8);
    for (std::size_t i = 0; i < 8; ++i) {
        auto& g = tm.nodeGeom[i];
        g.kind = cyber::remesh::bimdf::NodeGeomKind::Cone;
        g.coneIndex = 1;
        g.meshVertex = static_cast<std::uint32_t>(i);
        g.face = 0;
        g.position = {static_cast<float>(i & 1u), static_cast<float>((i >> 1) & 1u),
                      static_cast<float>((i >> 2) & 1u)};
    }
    tm.arcExcluded.assign(tm.arcs.size(), 0);

    cyber::remesh::bimdf::Charts charts;
    charts.faces.resize(1);
    charts.faceOfCompact = {0};
    charts.sourceFaceCount = 1;

    TopologyLayout layout = layoutFromTMesh(charts, tm);
    CHECK(layout.nodes.size() == 8);
    CHECK(layout.arcs.size() == 12);
    CHECK(layout.patches.size() == 6);
    CHECK(layout.singularityCount() == 8);
    CHECK(layout.nonQuadPatchCount() == 0);
    CHECK(layout.stats().totalIndex == 8);
    CHECK(layout.stats().featureArcs == 12);
    // Cones are the layout's fixed scaffolding.
    for (const auto& n : layout.nodes) {
        CHECK(n.locked);
    }
    // Feature arcs may not be moved by quantization.
    for (const auto& a : layout.arcs) {
        CHECK(a.kind == LayoutArcKind::Feature);
        CHECK(a.locked);
        CHECK(a.quantizedLength == -1);
    }
    CHECK(validateTopologyLayout(layout, 1).clean());

    // A quantization is recorded onto the layout in whole grid edges.
    cyber::remesh::bimdf::BimdfResult sol;
    sol.ok = true;
    sol.arcLenHalf.assign(tm.arcs.size(), 6);  // 6 half-cells == 3 grid edges
    applyQuantization(layout, tm, sol);
    for (const auto& a : layout.arcs) {
        CHECK(a.quantizedLength == 3);
    }
    for (const auto& p : layout.patches) {
        CHECK(p.uCount == 3);
        CHECK(p.vCount == 3);
    }
}

TEST_CASE("layout: a T-mesh traced without geometry still promotes") {
    cyber::remesh::bimdf::TMesh tm;
    tm.ok = true;
    tm.nodeCount = 2;
    cyber::remesh::bimdf::Arc arc;
    arc.n0 = 0;
    arc.n1 = 1;
    arc.len = 1.5;
    tm.arcs.push_back(arc);

    const cyber::remesh::bimdf::Charts charts;
    const TopologyLayout layout = layoutFromTMesh(charts, tm);
    REQUIRE(layout.nodes.size() == 2);
    REQUIRE(layout.arcs.size() == 1);
    CHECK(layout.arcs[0].samples.empty());
    CHECK(layout.arcs[0].desiredLength == doctest::Approx(1.5));
    // No patches means no incidence to check; the graph is still sound.
    CHECK(validateTopologyLayout(layout, 0).clean());
}

TEST_CASE("layout: a negative relaxed arc length is clamped, not propagated") {
    // Folded relaxed maps really do produce negative parametric spans; the
    // layout reports a length, so it must never go below zero (which would
    // otherwise trip the finite-non-negative invariant).
    cyber::remesh::bimdf::TMesh tm;
    tm.ok = true;
    tm.nodeCount = 2;
    cyber::remesh::bimdf::Arc arc;
    arc.n0 = 0;
    arc.n1 = 1;
    arc.len = -0.75;
    tm.arcs.push_back(arc);

    const cyber::remesh::bimdf::Charts charts;
    const TopologyLayout layout = layoutFromTMesh(charts, tm);
    CHECK(layout.arcs[0].desiredLength == 0.0);
    CHECK(validateTopologyLayout(layout, 0).ok);
}
