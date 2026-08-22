#include "cyber/uv/seam_path.hpp"

#include <algorithm>
#include <cmath>

#include "cyber/retopo/paths.hpp"
#include "cyber/retopo/picking.hpp"

namespace cyber::uv {

namespace {

// Weights are user-settable through the C ABI; Dijkstra needs strictly
// positive steps, so no configuration may push a multiplier to zero.
constexpr float kAbsoluteWeightFloor = 1e-6f;

float weightFloor(const SeamCostOptions& options) {
    return std::max(kAbsoluteWeightFloor, options.minWeight);
}

}  // namespace

float edgeSignedDihedral(const Mesh& mesh, EdgeId edge) {
    if (!edge.valid() || !mesh.isAlive(edge)) {
        return 0.0f;
    }
    const std::vector<FaceId> faces = mesh.edgeFaces(edge);
    if (faces.size() != 2) {
        return 0.0f;  // flat by convention: boundary and non-manifold edges
    }
    const Vec3 n0 = normalized(mesh.faceNormal(faces[0]));
    const Vec3 n1 = normalized(mesh.faceNormal(faces[1]));
    if (lengthSquared(n0) == 0.0f || lengthSquared(n1) == 0.0f) {
        return 0.0f;  // degenerate face
    }
    const float angle = radiansToDegrees(std::acos(std::fmax(-1.0f, std::fmin(1.0f, dot(n0, n1)))));
    const Vec3 c0 = mesh.faceCentroid(faces[0]);
    const Vec3 c1 = mesh.faceCentroid(faces[1]);
    // Symmetric centroid test: each face's normal leans TOWARD the other face
    // inside a valley and AWAY from it over a ridge.
    const float openness = dot(n0, c1 - c0) + dot(n1, c0 - c1);
    return openness < 0.0f ? -angle : angle;
}

float seamEdgeCost(const Mesh& mesh, EdgeId edge, const SeamCostOptions& options) {
    const auto [a, b] = mesh.edgeVertices(edge);
    const float len = length(mesh.position(b) - mesh.position(a));
    float weight = options.flatWeight;
    if (mesh.isFeatureEdge(edge)) {
        weight = std::fmin(weight, options.featureWeight);
    }
    const float dihedral = edgeSignedDihedral(mesh, edge);
    if (dihedral >= options.creaseDegrees) {
        weight = std::fmin(weight, options.concaveWeight);
    } else if (-dihedral >= options.creaseDegrees) {
        weight = std::fmin(weight, options.convexWeight);
    }
    return len * std::fmax(weightFloor(options), weight);
}

std::vector<VertexId> routeSeamSegment(const Mesh& mesh, VertexId from, VertexId to,
                                       const SeamCostOptions& options) {
    return retopo::weightedVertexPath(mesh, from, to, [&](EdgeId e, VertexId, VertexId) {
        return seamEdgeCost(mesh, e, options);
    });
}

void revertCommit(SeamSet& seams, const SeamCommit& commit) {
    for (const EdgeId edge : commit.markedEdges) {
        seams.erase(edge);
    }
}

// ---- SeamPath -----------------------------------------------------------

void SeamPath::routeSegment(std::size_t index) {
    Segment& seg = m_segments[index];
    seg.vertices = routeSeamSegment(*m_mesh, m_waypoints[index], m_waypoints[index + 1], m_options);
    seg.routed = !seg.vertices.empty();
    ++seg.routeRevision;
}

// Re-routes only the at most two segments that touch `waypointIndex` — the
// local re-route isolation the spec's "Fix a deviating waypoint" scenario
// requires. Every other segment keeps both its vertices and its revision.
void SeamPath::rerouteAround(std::size_t waypointIndex) {
    if (waypointIndex > 0) {
        routeSegment(waypointIndex - 1);
    }
    if (waypointIndex < m_segments.size()) {
        routeSegment(waypointIndex);
    }
}

bool SeamPath::addWaypoint(VertexId vertex) {
    if (!m_mesh->isAlive(vertex)) {
        return false;
    }
    // The armed resume marker acts as the current last waypoint, so the repeat
    // check has to run against it BEFORE it is seeded — otherwise a rejected
    // add still grows the pending path.
    const bool seedResume = m_waypoints.empty() && m_resume.valid() && m_mesh->isAlive(m_resume);
    const VertexId last =
        m_waypoints.empty() ? (seedResume ? m_resume : VertexId{}) : m_waypoints.back();
    if (last.valid() && last == vertex) {
        return false;
    }
    if (seedResume) {
        m_waypoints.push_back(m_resume);
    }
    m_waypoints.push_back(vertex);
    if (m_waypoints.size() < 2) {
        return true;
    }
    m_segments.emplace_back();
    routeSegment(m_segments.size() - 1);
    return true;
}

bool SeamPath::moveWaypoint(std::size_t index, VertexId vertex) {
    if (index >= m_waypoints.size() || !m_mesh->isAlive(vertex) || m_waypoints[index] == vertex) {
        return false;
    }
    m_waypoints[index] = vertex;
    rerouteAround(index);
    return true;
}

bool SeamPath::moveWaypointTo(std::size_t index, Vec3 position, float radius) {
    if (index >= m_waypoints.size()) {
        return false;
    }
    const std::optional<retopo::VertexPick> pick = retopo::nearestVertex(*m_mesh, position, radius);
    return pick && moveWaypoint(index, pick->vertex);
}

bool SeamPath::removeWaypoint(std::size_t index) {
    const std::size_t count = m_waypoints.size();
    if (index >= count) {
        return false;
    }
    m_waypoints.erase(m_waypoints.begin() + static_cast<std::ptrdiff_t>(index));
    if (m_segments.empty()) {
        return true;
    }
    if (index == 0) {
        m_segments.erase(m_segments.begin());
    } else if (index + 1 == count) {
        m_segments.pop_back();
    } else {
        // Interior: the two segments that met at the waypoint merge into one
        // re-routed span; every other segment keeps its route and revision.
        m_segments.erase(m_segments.begin() + static_cast<std::ptrdiff_t>(index));
        routeSegment(index - 1);
    }
    return true;
}

void SeamPath::clearPending() {
    m_waypoints.clear();
    m_segments.clear();
}

// Unrouted segments contribute nothing, so a partially routed path yields a
// chain with a gap; pendingEdges() drops the bogus jump, and commit() refuses
// such a path outright.
std::vector<VertexId> SeamPath::vertices() const {
    std::vector<VertexId> chain;
    for (const Segment& seg : m_segments) {
        if (!seg.routed) {
            continue;
        }
        const std::size_t skip = chain.empty() ? 0 : 1;  // the shared joint
        chain.insert(chain.end(), seg.vertices.begin() + static_cast<std::ptrdiff_t>(skip),
                     seg.vertices.end());
    }
    return chain;
}

std::vector<EdgeId> SeamPath::pendingEdges() const {
    const std::vector<VertexId> chain = vertices();
    std::vector<EdgeId> edges;
    for (std::size_t i = 1; i < chain.size(); ++i) {
        const EdgeId edge = m_mesh->edgeBetween(chain[i - 1], chain[i]);
        if (edge.valid() && m_mesh->isAlive(edge)) {
            edges.push_back(edge);
        }
    }
    return edges;
}

bool SeamPath::routed() const {
    return !m_segments.empty() && std::all_of(m_segments.begin(), m_segments.end(),
                                              [](const Segment& seg) { return seg.routed; });
}

SeamCommit SeamPath::commit(SeamSet& seams) {
    SeamCommit record;
    if (!routed()) {
        return record;  // left pending so the broken segment can be repaired
    }
    for (const EdgeId edge : pendingEdges()) {
        if (!seams.isSeam(edge)) {
            seams.mark(edge);
            record.markedEdges.push_back(edge);
        }
    }
    const std::vector<VertexId> chain = vertices();
    record.lastVertex = chain.back();
    m_resume = record.lastVertex;
    clearPending();
    return record;
}

}  // namespace cyber::uv
