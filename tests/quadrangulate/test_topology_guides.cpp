#include <doctest.h>

#include <vector>

#include "cyber/core/guidance.hpp"
#include "cyber/core/mesh.hpp"
#include "cyber/quadrangulate/topology_guides.hpp"

using cyber::Index;
using cyber::Mesh;
using cyber::Vec3;
using cyber::VertexId;
using cyber::remesh::FlowGuide;
using cyber::remesh::GuideMode;
using cyber::remesh::GuidePath;
using cyber::remesh::insertGuideArcs;
using cyber::remesh::LayoutArcKind;
using cyber::remesh::LayoutNodeKind;
using cyber::remesh::measureGuideAdherence;
using cyber::remesh::projectGuideToPath;
using cyber::remesh::TopologyLayout;

namespace {

// n x n unit-spaced quad grid in the z = 0 plane.
Mesh grid(int n) {
    std::vector<Vec3> p;
    for (int y = 0; y <= n; ++y) {
        for (int x = 0; x <= n; ++x) {
            p.push_back(Vec3{static_cast<float>(x), static_cast<float>(y), 0.0f});
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

FlowGuide topologyGuide(std::vector<Vec3> points, bool closed = false) {
    FlowGuide g;
    g.points = std::move(points);
    g.mode = GuideMode::Topology;
    g.closed = closed;
    g.radius = 0.5f;
    return g;
}

}  // namespace

TEST_CASE("guide projection: a straight stroke becomes a connected edge path") {
    const Mesh mesh = grid(4);
    // Along the y = 2 row, sampled coarsely — only the two ends.
    const GuidePath path =
        projectGuideToPath(mesh, topologyGuide({{0.0f, 2.0f, 0.0f}, {4.0f, 2.0f, 0.0f}}));
    REQUIRE(path.vertices.size() >= 2);
    // The point of joining rather than snapping: a stroke sampled more coarsely
    // than the mesh must still come back as a path with an edge between every
    // consecutive pair, or no edge chain can follow it.
    CHECK(path.edges.size() == path.vertices.size() - 1);
    for (std::size_t i = 0; i + 1 < path.vertices.size(); ++i) {
        CHECK(mesh.edgeBetween(path.vertices[i], path.vertices[i + 1]).valid());
    }
    // It followed the row it was drawn on.
    for (const VertexId v : path.vertices) {
        CHECK(mesh.position(v).y == doctest::Approx(2.0f));
    }
    CHECK_FALSE(path.closed);
    CHECK(path.maxDeviation == doctest::Approx(0.0f).epsilon(1e-5));
}

TEST_CASE("guide projection: a closed stroke comes back closed") {
    const Mesh mesh = grid(4);
    const GuidePath path = projectGuideToPath(
        mesh, topologyGuide(
                  {{1.0f, 1.0f, 0.0f}, {3.0f, 1.0f, 0.0f}, {3.0f, 3.0f, 0.0f}, {1.0f, 3.0f, 0.0f}},
                  true));
    REQUIRE(path.vertices.size() >= 4);
    CHECK(path.closed);
    CHECK(path.vertices.front() == path.vertices.back());
    for (std::size_t i = 0; i + 1 < path.vertices.size(); ++i) {
        CHECK(mesh.edgeBetween(path.vertices[i], path.vertices[i + 1]).valid());
    }
}

TEST_CASE("guide projection reports how far it had to stray") {
    const Mesh mesh = grid(4);
    // A stroke through the middle of a quad row: the mesh has no vertices
    // there, so the path must sit half a cell off — and say so, rather than
    // silently returning a path that does not follow the stroke.
    const GuidePath path =
        projectGuideToPath(mesh, topologyGuide({{0.0f, 1.5f, 0.0f}, {4.0f, 1.5f, 0.0f}}));
    REQUIRE(path.vertices.size() >= 2);
    CHECK(path.maxDeviation > 0.4f);
    CHECK(path.maxDeviation < 0.6f);
}

TEST_CASE("guide projection is deterministic") {
    const Mesh mesh = grid(5);
    const FlowGuide guide = topologyGuide({{0.0f, 2.0f, 0.0f}, {5.0f, 3.0f, 0.0f}});
    const GuidePath first = projectGuideToPath(mesh, guide);
    REQUIRE_FALSE(first.vertices.empty());
    for (int i = 0; i < 4; ++i) {
        const GuidePath again = projectGuideToPath(mesh, guide);
        CHECK(again.vertices == first.vertices);
        CHECK(again.edges == first.edges);
    }
}

TEST_CASE("guide projection declines rather than guessing") {
    const Mesh mesh = grid(3);
    CHECK(projectGuideToPath(mesh, topologyGuide({{0.0f, 0.0f, 0.0f}})).vertices.empty());
    CHECK(projectGuideToPath(mesh, topologyGuide({})).vertices.empty());
    const Mesh empty;
    CHECK(projectGuideToPath(empty, topologyGuide({{0, 0, 0}, {1, 1, 1}})).vertices.empty());
}

TEST_CASE("guide arcs enter the layout as first-class, locked structure") {
    const Mesh mesh = grid(4);
    const GuidePath open =
        projectGuideToPath(mesh, topologyGuide({{0.0f, 2.0f, 0.0f}, {4.0f, 2.0f, 0.0f}}));
    REQUIRE(open.vertices.size() >= 2);

    TopologyLayout layout;
    insertGuideArcs(layout, open);
    CHECK(layout.nodes.size() == open.vertices.size());
    CHECK(layout.arcs.size() == open.vertices.size() - 1);
    for (const auto& n : layout.nodes) {
        CHECK(n.kind == LayoutNodeKind::GuideAnchor);
        // An artist put this here: nothing downstream may relocate it.
        CHECK(n.locked);
    }
    for (const auto& a : layout.arcs) {
        CHECK(a.kind == LayoutArcKind::Guide);
        CHECK(a.locked);
    }
}

TEST_CASE("a closed guide contributes one arc per node, not one fewer") {
    const Mesh mesh = grid(4);
    const GuidePath loop = projectGuideToPath(
        mesh, topologyGuide(
                  {{1.0f, 1.0f, 0.0f}, {3.0f, 1.0f, 0.0f}, {3.0f, 3.0f, 0.0f}, {1.0f, 3.0f, 0.0f}},
                  true));
    REQUIRE(loop.closed);
    TopologyLayout layout;
    insertGuideArcs(layout, loop);
    // The repeated first vertex becomes the closing ARC, not a duplicate node.
    CHECK(layout.nodes.size() == loop.vertices.size() - 1);
    CHECK(layout.arcs.size() == layout.nodes.size());
    CHECK(layout.arcs.back().end == layout.arcs.front().begin);
}

TEST_CASE("guide adherence is measured against output EDGES, not vertices") {
    // A vertex landing near the stroke proves nothing; a chain of edges running
    // along it is what "the loop followed my stroke" means. A guide laid
    // exactly on a grid row is fully covered; one laid through the middle of a
    // row of quads is not, even though vertices sit close by on both sides.
    const Mesh mesh = grid(6);
    const FlowGuide onRow = topologyGuide({{0.0f, 3.0f, 0.0f}, {6.0f, 3.0f, 0.0f}});
    const FlowGuide betweenRows = topologyGuide({{0.0f, 3.5f, 0.0f}, {6.0f, 3.5f, 0.0f}});

    const auto covered = measureGuideAdherence(mesh, onRow, 0.05f);
    const auto missed = measureGuideAdherence(mesh, betweenRows, 0.05f);
    CHECK(covered.edgeChainCoverage == doctest::Approx(1.0f));
    CHECK(covered.maxDistance < 0.05f);
    CHECK(missed.edgeChainCoverage == doctest::Approx(0.0f));
    // And this is exactly why alignment has to be part of the test: the missed
    // guide is SURROUNDED by edges — the column edges cross it every cell, so
    // the nearest edge of any orientation is only a quarter cell away. A metric
    // built on proximity alone would have called this a good result.
    CHECK(missed.meanDistance < 0.3f);
}

TEST_CASE("guide mode defaults to orientation, so old guides are unchanged") {
    // Byte-compatibility for every guide authored before the mode existed.
    const FlowGuide plain;
    CHECK(plain.mode == GuideMode::Orientation);
    CHECK_FALSE(plain.closed);

    cyber::remesh::Guidance guidance;
    guidance.guides.push_back(plain);
    CHECK(guidance.topologyGuideCount() == 0);
    guidance.guides.push_back(topologyGuide({{0, 0, 0}, {1, 0, 0}}));
    CHECK(guidance.topologyGuideCount() == 1);
}
