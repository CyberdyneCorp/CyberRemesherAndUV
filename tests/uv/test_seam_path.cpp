#include <doctest.h>

#include <algorithm>
#include <vector>

#include "cyber/core/math.hpp"
#include "cyber/core/mesh.hpp"
#include "cyber/retopo/paths.hpp"
#include "cyber/uv/seam_path.hpp"
#include "cyber/uv/seams.hpp"
#include "cyber/uv/unwrap.hpp"

using cyber::EdgeId;
using cyber::FaceId;
using cyber::Index;
using cyber::Mesh;
using cyber::Vec3;
using cyber::VertexId;
namespace retopo = cyber::retopo;
namespace uv = cyber::uv;

namespace {

constexpr Index kCols = 7;  // x = 0..6
constexpr Index kRows = 5;  // y = 0..4

Index gridVertex(Index x, Index y) { return y * kCols + x; }

// A (kCols x kRows) quad grid in the plane z = 0. `grooveDepth` lowers the
// whole y == 1 row, turning the edges along it into a concave valley while the
// edges that climb its walls stay flat (they lie inside one sloping plane) and
// the y == 2 row becomes a convex break.
Mesh makeGrid(float grooveDepth) {
    std::vector<Vec3> p;
    for (Index y = 0; y < kRows; ++y) {
        for (Index x = 0; x < kCols; ++x) {
            const float z = y == 1 ? -grooveDepth : 0.0f;
            p.push_back({static_cast<float>(x), static_cast<float>(y), z});
        }
    }
    std::vector<std::vector<Index>> f;
    for (Index y = 0; y + 1 < kRows; ++y) {
        for (Index x = 0; x + 1 < kCols; ++x) {
            f.push_back({gridVertex(x, y), gridVertex(x + 1, y), gridVertex(x + 1, y + 1),
                         gridVertex(x, y + 1)});
        }
    }
    return Mesh::fromIndexed(p, f);
}

Mesh makeGrooveGrid() { return makeGrid(0.5f); }
Mesh makeFlatGrid() { return makeGrid(0.0f); }

// The dog-leg that follows the groove: down the left wall, along the valley
// floor, back up the right wall.
std::vector<VertexId> grooveChain() {
    std::vector<VertexId> chain{VertexId{gridVertex(0, 2)}};
    for (Index x = 0; x < kCols; ++x) {
        chain.push_back(VertexId{gridVertex(x, 1)});
    }
    chain.push_back(VertexId{gridVertex(kCols - 1, 2)});
    return chain;
}

float routeCost(const Mesh& mesh, const std::vector<VertexId>& chain,
                const uv::SeamCostOptions& options) {
    float total = 0.0f;
    for (std::size_t i = 1; i < chain.size(); ++i) {
        const EdgeId e = mesh.edgeBetween(chain[i - 1], chain[i]);
        REQUIRE(e.valid());
        total += uv::seamEdgeCost(mesh, e, options);
    }
    return total;
}

float routeLength(const Mesh& mesh, const std::vector<VertexId>& chain) {
    float total = 0.0f;
    for (std::size_t i = 1; i < chain.size(); ++i) {
        total += cyber::length(mesh.position(chain[i]) - mesh.position(chain[i - 1]));
    }
    return total;
}

// Two quads hinged about the shared edge (1,0)-(1,1); `lift` raises the far
// side of the second quad (valley) or lowers it (ridge).
Mesh makeHinge(float lift) {
    const std::vector<Vec3> p = {{0, 0, lift}, {1, 0, 0},    {1, 1, 0},
                                 {0, 1, lift}, {2, 0, lift}, {2, 1, lift}};
    const std::vector<std::vector<Index>> f = {{0, 1, 2, 3}, {1, 4, 5, 2}};
    return Mesh::fromIndexed(p, f);
}

Mesh makeTwoComponents() {
    const std::vector<Vec3> p = {{0, 0, 0}, {1, 0, 0}, {1, 1, 0}, {0, 1, 0},
                                 {5, 0, 0}, {6, 0, 0}, {6, 1, 0}, {5, 1, 0}};
    const std::vector<std::vector<Index>> f = {{0, 1, 2, 3}, {4, 5, 6, 7}};
    return Mesh::fromIndexed(p, f);
}

}  // namespace

// ---- cost model ---------------------------------------------------------

TEST_CASE("signed dihedral is positive in a valley, negative on a ridge, zero when flat") {
    const Mesh valley = makeHinge(0.5f);
    const Mesh ridge = makeHinge(-0.5f);
    const Mesh flat = makeHinge(0.0f);

    const EdgeId hingeV = valley.edgeBetween(VertexId{1}, VertexId{2});
    const EdgeId hingeR = ridge.edgeBetween(VertexId{1}, VertexId{2});
    const EdgeId hingeF = flat.edgeBetween(VertexId{1}, VertexId{2});
    REQUIRE(hingeV.valid());

    CHECK(uv::edgeSignedDihedral(valley, hingeV) > 20.0f);
    CHECK(uv::edgeSignedDihedral(ridge, hingeR) < -20.0f);
    CHECK(uv::edgeSignedDihedral(flat, hingeF) == doctest::Approx(0.0f));
    // The two are mirror images, so the magnitudes must agree exactly.
    CHECK(uv::edgeSignedDihedral(valley, hingeV) ==
          doctest::Approx(-uv::edgeSignedDihedral(ridge, hingeR)));

    // Boundary edges carry no dihedral.
    const EdgeId border = valley.edgeBetween(VertexId{0}, VertexId{1});
    REQUIRE(border.valid());
    REQUIRE(valley.isBoundaryEdge(border));
    CHECK(uv::edgeSignedDihedral(valley, border) == doctest::Approx(0.0f));
}

TEST_CASE("seam edge cost discounts feature and concave edges, never below a positive floor") {
    Mesh mesh = makeGrooveGrid();
    const uv::SeamCostOptions options;

    const EdgeId groove = mesh.edgeBetween(VertexId{gridVertex(2, 1)}, VertexId{gridVertex(3, 1)});
    const EdgeId plain = mesh.edgeBetween(VertexId{gridVertex(2, 3)}, VertexId{gridVertex(3, 3)});
    REQUIRE(groove.valid());
    REQUIRE(plain.valid());
    // Same unit length; the concavity discount is the only difference.
    REQUIRE(uv::edgeSignedDihedral(mesh, groove) > options.creaseDegrees);
    CHECK(uv::seamEdgeCost(mesh, groove, options) < uv::seamEdgeCost(mesh, plain, options));

    // Tagging the plain edge discounts it too — without any curvature.
    const float untagged = uv::seamEdgeCost(mesh, plain, options);
    mesh.setFeatureEdge(plain, true);
    CHECK(uv::seamEdgeCost(mesh, plain, options) < untagged);

    // A degenerate configuration must still produce a strictly positive cost:
    // Dijkstra is only correct for non-negative weights, and a zero-weight
    // frontier would let the router wander for free.
    uv::SeamCostOptions broken;
    broken.flatWeight = -5.0f;
    broken.featureWeight = 0.0f;
    broken.concaveWeight = -1.0f;
    broken.minWeight = 0.0f;
    CHECK(uv::seamEdgeCost(mesh, groove, broken) > 0.0f);
    CHECK(uv::seamEdgeCost(mesh, plain, broken) > 0.0f);
}

// ---- Scenario: Route follows the groove ---------------------------------

TEST_CASE("routed seam path follows a concave groove instead of the flat shortcut") {
    const Mesh mesh = makeGrooveGrid();
    const VertexId from{gridVertex(0, 2)};
    const VertexId to{gridVertex(kCols - 1, 2)};
    const uv::SeamCostOptions options;
    const std::vector<VertexId> groove = grooveChain();

    // (a) The bias is doing the work, not the geometry: unweighted, the flat
    // run straight across is strictly shorter and is what Dijkstra picks.
    const std::vector<VertexId> geodesic = retopo::shortestVertexPath(mesh, from, to);
    REQUIRE(geodesic.size() == kCols);
    CHECK(geodesic != groove);
    CHECK(routeLength(mesh, geodesic) < routeLength(mesh, groove));

    // (b) With the seam cost model the route drops into the valley.
    const std::vector<VertexId> routed = uv::routeSeamSegment(mesh, from, to, options);
    CHECK(routed == groove);

    // (c) ... because the groove is strictly cheaper under that cost model.
    CHECK(routeCost(mesh, groove, options) < routeCost(mesh, geodesic, options));

    // The tool-level entry point agrees with the free function.
    uv::SeamPath path(mesh, options);
    REQUIRE(path.addWaypoint(from));
    REQUIRE(path.addWaypoint(to));
    REQUIRE(path.routed());
    CHECK(path.vertices() == groove);
}

TEST_CASE("a feature-tagged detour is preferred over an untagged straight run") {
    Mesh mesh = makeFlatGrid();  // no curvature anywhere: the tag alone steers
    const VertexId from{gridVertex(0, 2)};
    const VertexId to{gridVertex(kCols - 1, 2)};
    const std::vector<VertexId> detour = grooveChain();  // same dog-leg, now flat
    for (std::size_t i = 1; i < detour.size(); ++i) {
        const EdgeId e = mesh.edgeBetween(detour[i - 1], detour[i]);
        REQUIRE(e.valid());
        mesh.setFeatureEdge(e, true);
    }

    const uv::SeamCostOptions options;
    CHECK(retopo::shortestVertexPath(mesh, from, to) != detour);
    CHECK(uv::routeSeamSegment(mesh, from, to, options) == detour);
}

// ---- Scenario: Fix a deviating waypoint ---------------------------------

TEST_CASE("moving a waypoint re-routes only its adjacent segments") {
    const Mesh mesh = makeFlatGrid();
    uv::SeamPath path(mesh);
    REQUIRE(path.addWaypoint(VertexId{gridVertex(0, 0)}));
    REQUIRE(path.addWaypoint(VertexId{gridVertex(2, 0)}));
    REQUIRE(path.addWaypoint(VertexId{gridVertex(4, 0)}));
    REQUIRE(path.addWaypoint(VertexId{gridVertex(6, 0)}));
    REQUIRE(path.segmentCount() == 3);
    REQUIRE(path.routed());

    const std::vector<VertexId> before0 = path.segment(0).vertices;
    const std::vector<VertexId> before2 = path.segment(2).vertices;
    const std::uint64_t rev0 = path.segment(0).routeRevision;
    const std::uint64_t rev1 = path.segment(1).routeRevision;
    const std::uint64_t rev2 = path.segment(2).routeRevision;

    // Drag the interior waypoint 1 off the straight run.
    REQUIRE(path.moveWaypoint(1, VertexId{gridVertex(2, 3)}));

    // The two segments that touch it re-routed...
    CHECK(path.segment(0).vertices != before0);
    CHECK(path.segment(0).routeRevision == rev0 + 1);
    CHECK(path.segment(1).routeRevision == rev1 + 1);
    CHECK(path.segment(0).vertices.back() == VertexId{gridVertex(2, 3)});
    CHECK(path.segment(1).vertices.front() == VertexId{gridVertex(2, 3)});
    // ... and the rest of the pending path is untouched: same vertices AND no
    // re-route at all (the counter would have moved).
    CHECK(path.segment(2).vertices == before2);
    CHECK(path.segment(2).routeRevision == rev2);
}

TEST_CASE("moving an endpoint waypoint re-routes exactly one segment") {
    const Mesh mesh = makeFlatGrid();
    uv::SeamPath path(mesh);
    REQUIRE(path.addWaypoint(VertexId{gridVertex(0, 0)}));
    REQUIRE(path.addWaypoint(VertexId{gridVertex(3, 0)}));
    REQUIRE(path.addWaypoint(VertexId{gridVertex(6, 0)}));
    const std::uint64_t rev0 = path.segment(0).routeRevision;
    const std::uint64_t rev1 = path.segment(1).routeRevision;

    REQUIRE(path.moveWaypoint(0, VertexId{gridVertex(0, 4)}));
    CHECK(path.segment(0).routeRevision == rev0 + 1);
    CHECK(path.segment(1).routeRevision == rev1);
    CHECK(path.segment(0).vertices.front() == VertexId{gridVertex(0, 4)});

    REQUIRE(path.moveWaypoint(2, VertexId{gridVertex(6, 4)}));
    CHECK(path.segment(0).routeRevision == rev0 + 1);  // still untouched
    CHECK(path.segment(1).routeRevision == rev1 + 1);
}

TEST_CASE("moveWaypointTo snaps the drag onto the nearest vertex, or does nothing") {
    const Mesh mesh = makeFlatGrid();
    uv::SeamPath path(mesh);
    REQUIRE(path.addWaypoint(VertexId{gridVertex(0, 0)}));
    REQUIRE(path.addWaypoint(VertexId{gridVertex(6, 0)}));
    const std::uint64_t rev0 = path.segment(0).routeRevision;

    // Nothing within range: the path must be left exactly as it was.
    CHECK_FALSE(path.moveWaypointTo(1, Vec3{6.0f, 0.0f, 50.0f}, 0.1f));
    CHECK(path.waypoints()[1] == VertexId{gridVertex(6, 0)});
    CHECK(path.segment(0).routeRevision == rev0);

    CHECK(path.moveWaypointTo(1, Vec3{5.9f, 3.95f, 0.0f}, 0.5f));
    CHECK(path.waypoints()[1] == VertexId{gridVertex(6, 4)});
    CHECK(path.segment(0).routeRevision == rev0 + 1);
}

TEST_CASE("deleting a waypoint merges its two adjacent segments into one re-route") {
    const Mesh mesh = makeFlatGrid();
    uv::SeamPath path(mesh);
    REQUIRE(path.addWaypoint(VertexId{gridVertex(0, 0)}));
    REQUIRE(path.addWaypoint(VertexId{gridVertex(3, 4)}));  // the deviating one
    REQUIRE(path.addWaypoint(VertexId{gridVertex(6, 0)}));
    REQUIRE(path.addWaypoint(VertexId{gridVertex(6, 4)}));
    REQUIRE(path.segmentCount() == 3);
    const std::vector<VertexId> tail = path.segment(2).vertices;
    const std::uint64_t tailRev = path.segment(2).routeRevision;

    REQUIRE(path.removeWaypoint(1));
    REQUIRE(path.waypointCount() == 3);
    REQUIRE(path.segmentCount() == 2);
    // The merged segment now spans the two surviving neighbours directly.
    CHECK(path.segment(0).vertices.front() == VertexId{gridVertex(0, 0)});
    CHECK(path.segment(0).vertices.back() == VertexId{gridVertex(6, 0)});
    CHECK(path.segment(0).vertices.size() == kCols);  // straight again
    // The untouched tail keeps both its route and its revision.
    CHECK(path.segment(1).vertices == tail);
    CHECK(path.segment(1).routeRevision == tailRev);

    // Deleting an endpoint just drops its segment.
    REQUIRE(path.removeWaypoint(2));
    CHECK(path.segmentCount() == 1);
    CHECK(path.segment(0).vertices.back() == VertexId{gridVertex(6, 0)});
}

// ---- Scenario: Commit then resume ---------------------------------------

TEST_CASE("commit arms a resume marker and dropping it leaves committed seams intact") {
    const Mesh mesh = makeGrooveGrid();
    const VertexId from{gridVertex(0, 2)};
    const VertexId to{gridVertex(kCols - 1, 2)};

    uv::SeamSet seams;
    uv::SeamPath path(mesh);
    REQUIRE(path.addWaypoint(from));
    REQUIRE(path.addWaypoint(to));
    const std::vector<EdgeId> expected = path.pendingEdges();
    REQUIRE(expected.size() == grooveChain().size() - 1);

    const uv::SeamCommit record = path.commit(seams);
    CHECK(record.markedEdges.size() == expected.size());
    for (const EdgeId e : expected) {
        CHECK(seams.isSeam(e));
    }
    // Committing consumes the pending path and arms the marker at its end.
    CHECK(path.waypointCount() == 0);
    CHECK(path.segmentCount() == 0);
    REQUIRE(path.hasResumeMarker());
    CHECK(path.resumeMarker() == to);
    CHECK(record.lastVertex == to);

    // Snapshot the committed seam state so the drop case can be compared to it.
    const std::size_t committedSize = seams.size();
    std::vector<bool> committed(mesh.edgeCapacity(), false);
    for (Index i = 0; i < mesh.edgeCapacity(); ++i) {
        committed[i] = seams.isSeam(EdgeId{i});
    }

    SUBCASE("resuming starts the new route at the last committed point") {
        REQUIRE(path.addWaypoint(VertexId{gridVertex(kCols - 1, 4)}));
        REQUIRE(path.segmentCount() == 1);
        CHECK(path.waypoints().front() == to);
        CHECK(path.segment(0).vertices.front() == to);
        CHECK(path.segment(0).vertices.back() == VertexId{gridVertex(kCols - 1, 4)});
    }

    SUBCASE("dropping the marker starts a fresh path and changes no committed seam") {
        path.dropResumeMarker();
        CHECK_FALSE(path.hasResumeMarker());
        REQUIRE(path.addWaypoint(VertexId{gridVertex(0, 4)}));
        REQUIRE(path.waypointCount() == 1);  // not seeded at the old end
        CHECK(path.waypoints().front() == VertexId{gridVertex(0, 4)});
        CHECK(path.segmentCount() == 0);

        CHECK(seams.size() == committedSize);
        for (Index i = 0; i < mesh.edgeCapacity(); ++i) {
            CHECK(seams.isSeam(EdgeId{i}) == committed[i]);
        }
    }
}

TEST_CASE("committing a routed seam splits the mesh into a new island that unwrap can unfold") {
    Mesh mesh = makeFlatGrid();
    uv::SeamSet seams;
    REQUIRE(uv::computeIslands(mesh, seams).size() == 1);

    uv::SeamPath path(mesh);
    REQUIRE(path.addWaypoint(VertexId{gridVertex(3, 0)}));
    REQUIRE(path.addWaypoint(VertexId{gridVertex(3, kRows - 1)}));
    REQUIRE(path.routed());
    const uv::SeamCommit record = path.commit(seams);
    REQUIRE(record.markedEdges.size() == kRows - 1);

    // The routed seam behaves exactly like a hand-drawn one under the seam
    // model: a new island appears and gesture unwrap unfolds it.
    const std::vector<std::vector<FaceId>> islands = uv::computeIslands(mesh, seams);
    REQUIRE(islands.size() == 2);
    for (const std::vector<FaceId>& island : islands) {
        CHECK(uv::unwrapIslandToUv(mesh, island));
    }
}

TEST_CASE("an unroutable segment leaves the path unrouted and commits nothing") {
    const Mesh mesh = makeTwoComponents();
    uv::SeamSet seams;
    uv::SeamPath path(mesh);
    REQUIRE(path.addWaypoint(VertexId{0}));
    REQUIRE(path.addWaypoint(VertexId{5}));  // other component
    REQUIRE(path.segmentCount() == 1);
    CHECK_FALSE(path.segment(0).routed);
    CHECK_FALSE(path.routed());

    const uv::SeamCommit record = path.commit(seams);
    CHECK(record.empty());
    CHECK(seams.empty());
    CHECK_FALSE(path.hasResumeMarker());
    // The broken path stays pending so the waypoint can be repaired.
    CHECK(path.waypointCount() == 2);
    REQUIRE(path.moveWaypoint(1, VertexId{2}));
    CHECK(path.routed());
    CHECK_FALSE(path.commit(seams).empty());
}

TEST_CASE("revertCommit restores the exact pre-commit seam set") {
    const Mesh mesh = makeFlatGrid();
    uv::SeamSet seams;
    // An edge the user had already seamed by hand, lying ON the routed path.
    const EdgeId preexisting =
        mesh.edgeBetween(VertexId{gridVertex(3, 1)}, VertexId{gridVertex(3, 2)});
    REQUIRE(preexisting.valid());
    seams.mark(preexisting);

    uv::SeamPath path(mesh);
    REQUIRE(path.addWaypoint(VertexId{gridVertex(3, 0)}));
    REQUIRE(path.addWaypoint(VertexId{gridVertex(3, kRows - 1)}));
    const std::vector<EdgeId> routedEdges = path.pendingEdges();
    REQUIRE(std::find(routedEdges.begin(), routedEdges.end(), preexisting) != routedEdges.end());

    const uv::SeamCommit record = path.commit(seams);
    // The already-seamed edge is not part of the undo record.
    CHECK(record.markedEdges.size() == routedEdges.size() - 1);
    CHECK(std::find(record.markedEdges.begin(), record.markedEdges.end(), preexisting) ==
          record.markedEdges.end());

    uv::revertCommit(seams, record);
    CHECK(seams.size() == 1);
    CHECK(seams.isSeam(preexisting));  // survives the revert
}

TEST_CASE("clearPending drops the pending path but keeps the resume marker") {
    const Mesh mesh = makeFlatGrid();
    uv::SeamSet seams;
    uv::SeamPath path(mesh);
    REQUIRE(path.addWaypoint(VertexId{gridVertex(0, 0)}));
    REQUIRE(path.addWaypoint(VertexId{gridVertex(6, 0)}));
    path.commit(seams);
    REQUIRE(path.hasResumeMarker());

    REQUIRE(path.addWaypoint(VertexId{gridVertex(6, 4)}));
    path.clearPending();
    CHECK(path.waypointCount() == 0);
    CHECK(path.segmentCount() == 0);
    CHECK(path.hasResumeMarker());
    CHECK(seams.size() == 6);  // committed seams untouched
}

TEST_CASE("waypoint edits reject dead vertices and no-op repeats") {
    const Mesh mesh = makeFlatGrid();
    uv::SeamPath path(mesh);
    CHECK_FALSE(path.addWaypoint(VertexId{}));
    CHECK_FALSE(path.addWaypoint(VertexId{9999}));
    REQUIRE(path.addWaypoint(VertexId{gridVertex(0, 0)}));
    CHECK_FALSE(path.addWaypoint(VertexId{gridVertex(0, 0)}));  // repeat
    CHECK(path.waypointCount() == 1);
    CHECK_FALSE(path.moveWaypoint(5, VertexId{gridVertex(1, 1)}));
    CHECK_FALSE(path.removeWaypoint(5));
    CHECK(path.removeWaypoint(0));
    CHECK(path.waypointCount() == 0);
}
