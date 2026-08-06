#pragma once

#include <cstddef>
#include <cstdint>
#include <vector>

#include "cyber/core/math.hpp"
#include "cyber/core/mesh.hpp"
#include "cyber/uv/seams.hpp"

// Auto-routed seam paths (uv-editing spec, "Auto-routed seam paths"). The user
// places waypoints on the EditMesh; between consecutive waypoints the engine
// routes a least-cost edge path whose cost prefers feature-tagged and concave
// (valley) edges over flat-region crossings, so short hops "follow the groove"
// instead of cutting the geodesic shortcut.
//
// This complements — it does not replace — stroke-over-edges seam editing:
// committing a path marks edges into the very same cyber::uv::SeamSet, so
// computeIslands, gesture unwrap and Erase-to-sew behave exactly as they do
// for hand-drawn seams.
namespace cyber::uv {

// Per-unit-length multipliers for the routing cost. Every weight is a
// multiplier on the edge's Euclidean length: 1 is "neutral", lower means
// "cheaper, prefer this edge". Weights are clamped to a strictly positive
// floor before use, so no setting can break Dijkstra's non-negative-weight
// requirement or stall the frontier.
struct SeamCostOptions {
    float flatWeight = 1.0f;      // baseline multiplier for an ordinary edge
    float featureWeight = 0.25f;  // multiplier for a feature-tagged edge
    float concaveWeight = 0.35f;  // multiplier for a valley edge past the crease angle
    float creaseDegrees = 20.0f;  // concavity (in degrees) that counts as a groove
    float minWeight = 1e-3f;      // floor applied to the chosen multiplier
};

// Signed dihedral angle of `edge` IN DEGREES: positive on a concave valley,
// negative on a convex ridge, 0 for flat, boundary and non-manifold edges
// (only edges with exactly two incident faces carry a dihedral).
//
// The sign comes from the symmetric centroid test dot(n0, c1 - c0) +
// dot(n1, c0 - c1), which needs no winding assumption beyond the mesh's own
// face normals. A mesh with INCONSISTENT winding reports the wrong sign for
// the edges between the mismatched faces; run the winding repair first if the
// input may be broken.
[[nodiscard]] float edgeSignedDihedral(const Mesh& mesh, EdgeId edge);

// Routing cost of traversing `edge`: its length times the cheapest applicable
// weight. Always strictly positive for a non-degenerate edge.
[[nodiscard]] float seamEdgeCost(const Mesh& mesh, EdgeId edge, const SeamCostOptions& options);

// Least-cost seam route between two live vertices under seamEdgeCost,
// endpoints inclusive and in from->to order. Empty when either vertex is
// dead, from == to, or the two are in different connected components.
[[nodiscard]] std::vector<VertexId> routeSeamSegment(const Mesh& mesh, VertexId from, VertexId to,
                                                     const SeamCostOptions& options);

// Undo record of one SeamPath::commit: exactly the edges the commit turned
// into seams (edges that were ALREADY seams are not listed, so reverting
// leaves them marked), plus the path's last vertex.
struct SeamCommit {
    std::vector<EdgeId> markedEdges;
    VertexId lastVertex;

    [[nodiscard]] bool empty() const { return markedEdges.empty(); }
};

// Restores the seam state as it was immediately before `commit`.
void revertCommit(SeamSet& seams, const SeamCommit& commit);

// A pending, editable seam path: an ordered list of waypoints plus one cached
// route per consecutive pair. Every waypoint can be repositioned or deleted
// until commit, and an edit re-routes ONLY the at most two segments adjacent
// to the touched waypoint (the `routeRevision` counters make that observable).
//
// Snapshot semantics, matching cyber::retopo::SurfaceSnapper: the path holds a
// reference to the mesh it was built on and caches vertex ids against it. It
// does not observe topology changes; rebuild the path if the mesh is edited
// underneath it. commit() re-resolves every edge through Mesh::edgeBetween
// rather than trusting the cache, so a stale path yields a short seam instead
// of corrupting the SeamSet.
class SeamPath {
public:
    struct Segment {
        std::vector<VertexId> vertices;   // waypoint i -> i+1, endpoints inclusive
        std::uint64_t routeRevision = 0;  // bumped each time THIS segment re-routes
        bool routed = false;              // false when no edge path connects the pair
    };

    explicit SeamPath(const Mesh& mesh, const SeamCostOptions& options = {})
        : m_mesh(&mesh), m_options(options) {}

    [[nodiscard]] const SeamCostOptions& options() const { return m_options; }
    void setOptions(const SeamCostOptions& options) { m_options = options; }

    // ---- editing ------------------------------------------------------
    // Appends a waypoint and routes the segment that closes onto it. With the
    // resume marker armed and no waypoints yet, the path is seeded at the
    // marker first, so the new route starts from the last committed point.
    // Returns false (changing nothing) for a dead vertex or a repeat of the
    // current last waypoint.
    bool addWaypoint(VertexId vertex);
    // Repositions waypoint `index`, re-routing only its adjacent segments.
    bool moveWaypoint(std::size_t index, VertexId vertex);
    // Repositions waypoint `index` onto the nearest live vertex within
    // `radius` of `position` (the drag gesture). False when nothing is in
    // range, leaving the path untouched.
    bool moveWaypointTo(std::size_t index, Vec3 position, float radius);
    // Deletes waypoint `index`; an interior deletion merges its two adjacent
    // segments into a single re-routed segment.
    bool removeWaypoint(std::size_t index);
    // Drops the pending waypoints and routes. Committed seams and the resume
    // marker are untouched.
    void clearPending();

    // ---- pending state ------------------------------------------------
    [[nodiscard]] const std::vector<VertexId>& waypoints() const { return m_waypoints; }
    [[nodiscard]] std::size_t waypointCount() const { return m_waypoints.size(); }
    [[nodiscard]] std::size_t segmentCount() const { return m_segments.size(); }
    [[nodiscard]] const Segment& segment(std::size_t index) const { return m_segments[index]; }
    // The whole pending path as one vertex chain (shared joints appear once).
    [[nodiscard]] std::vector<VertexId> vertices() const;
    // Live edges the pending path would turn into seams.
    [[nodiscard]] std::vector<EdgeId> pendingEdges() const;
    // True when there is at least one segment and every segment routed.
    [[nodiscard]] bool routed() const;

    // ---- commit / resume ----------------------------------------------
    // Turns the pending path into seam edges and arms the resume marker at the
    // path's last vertex, then clears the pending waypoints. A path that is
    // not fully routed commits nothing and is left pending for repair.
    SeamCommit commit(SeamSet& seams);
    [[nodiscard]] bool hasResumeMarker() const { return m_resume.valid(); }
    [[nodiscard]] VertexId resumeMarker() const { return m_resume; }
    // Forgets where the last commit ended, so the next waypoint starts a fresh
    // path. Never touches a SeamSet — committed seams are unaffected.
    void dropResumeMarker() { m_resume = VertexId{}; }

private:
    void routeSegment(std::size_t index);
    void rerouteAround(std::size_t waypointIndex);

    const Mesh* m_mesh;
    SeamCostOptions m_options;
    std::vector<VertexId> m_waypoints;
    std::vector<Segment> m_segments;  // size == max(waypoints - 1, 0)
    VertexId m_resume;
};

}  // namespace cyber::uv
