#include "cyber/quadrangulate/topology_guides.hpp"

#include <algorithm>
#include <cmath>
#include <limits>
#include <map>
#include <queue>
#include <string>
#include <utility>

namespace cyber::remesh {
namespace {

constexpr float kInf = std::numeric_limits<float>::infinity();

VertexId nearestVertex(const Mesh& mesh, Vec3 p) {
    VertexId best{};
    float bestDistance = kInf;
    for (Index v = 0; v < mesh.vertexCapacity(); ++v) {
        const VertexId vid{v};
        if (!mesh.isAlive(vid)) {
            continue;
        }
        const Vec3 d = mesh.position(vid) - p;
        const float distance = dot(d, d);
        // Strictly less, so the lowest id wins a tie and the snap is stable.
        if (distance < bestDistance) {
            bestDistance = distance;
            best = vid;
        }
    }
    return best;
}

// Shortest edge path between two vertices, by Dijkstra over edge lengths with
// ties broken on vertex id. Returns the vertices from `from` to `to`, or empty
// when they are not connected.
std::vector<VertexId> shortestPath(const Mesh& mesh, VertexId from, VertexId to) {
    if (!from.valid() || !to.valid()) {
        return {};
    }
    if (from == to) {
        return {from};
    }
    std::vector<float> dist(mesh.vertexCapacity(), kInf);
    std::vector<Index> parent(mesh.vertexCapacity(), kInvalidIndex);
    using Entry = std::pair<float, Index>;
    std::priority_queue<Entry, std::vector<Entry>, std::greater<>> queue;
    dist[from.value] = 0.0f;
    queue.push({0.0f, from.value});
    while (!queue.empty()) {
        const auto [d, v] = queue.top();
        queue.pop();
        if (d > dist[v]) {
            continue;
        }
        if (v == to.value) {
            break;
        }
        for (const EdgeId e : mesh.vertexEdges(VertexId{v})) {
            if (!mesh.isAlive(e)) {
                continue;
            }
            const auto [a, b] = mesh.edgeVertices(e);
            const VertexId other = a.value == v ? b : a;
            if (!other.valid() || other.value >= dist.size()) {
                continue;
            }
            const float step = length(mesh.position(other) - mesh.position(VertexId{v}));
            const float nd = d + step;
            if (nd < dist[other.value]) {
                dist[other.value] = nd;
                parent[other.value] = v;
                queue.push({nd, other.value});
            }
        }
    }
    if (dist[to.value] == kInf) {
        return {};
    }
    std::vector<VertexId> path{to};
    while (path.back() != from) {
        const Index p = parent[path.back().value];
        if (p == kInvalidIndex) {
            return {};
        }
        path.push_back(VertexId{p});
    }
    std::reverse(path.begin(), path.end());
    return path;
}

// Distance from `p` to the guide polyline, treated as segments.
float distanceToGuide(const std::vector<Vec3>& points, bool closed, Vec3 p) {
    if (points.empty()) {
        return kInf;
    }
    if (points.size() == 1) {
        return length(p - points[0]);
    }
    float best = kInf;
    const std::size_t segments = closed ? points.size() : points.size() - 1;
    for (std::size_t i = 0; i < segments; ++i) {
        const Vec3 a = points[i];
        const Vec3 b = points[(i + 1) % points.size()];
        const Vec3 ab = b - a;
        const float len2 = dot(ab, ab);
        const float t = len2 > 1e-20f ? std::clamp(dot(p - a, ab) / len2, 0.0f, 1.0f) : 0.0f;
        best = std::min(best, length(p - (a + ab * t)));
    }
    return best;
}

std::vector<EdgeId> semanticEdges(const Mesh& mesh, const std::vector<std::int32_t>& values) {
    std::vector<EdgeId> out;
    for (Index edge = 0; edge < mesh.edgeCapacity(); ++edge) {
        const EdgeId id{edge};
        if (!mesh.isAlive(id) || mesh.edgeFaceCount(id) != 2) {
            continue;
        }
        const auto faces = mesh.edgeFaces(id);
        if (faces[0].value < values.size() && faces[1].value < values.size() &&
            values[faces[0].value] != values[faces[1].value]) {
            out.push_back(id);
        }
    }
    return out;
}

std::vector<SemanticBoundaryRequest> collectAttributeBoundaries(
    const Mesh& mesh, const std::string& name, const std::vector<std::int32_t>& values) {
    std::vector<SemanticBoundaryRequest> out;
    const std::vector<EdgeId> edges = semanticEdges(mesh, values);
    std::vector<char> seen(mesh.edgeCapacity(), 0);
    for (const EdgeId seed : edges) {
        if (seen[seed.value]) {
            continue;
        }
        std::vector<EdgeId> component;
        std::map<Index, std::vector<EdgeId>> incident;
        std::queue<EdgeId> pending;
        pending.push(seed);
        seen[seed.value] = 1;
        while (!pending.empty()) {
            const EdgeId edge = pending.front();
            pending.pop();
            component.push_back(edge);
            const auto [a, b] = mesh.edgeVertices(edge);
            incident[a.value].push_back(edge);
            incident[b.value].push_back(edge);
            for (const VertexId vertex : {a, b}) {
                for (const EdgeId next : mesh.vertexEdges(vertex)) {
                    if (next.value >= seen.size() || seen[next.value] ||
                        std::find(edges.begin(), edges.end(), next) == edges.end()) {
                        continue;
                    }
                    seen[next.value] = 1;
                    pending.push(next);
                }
            }
        }
        std::sort(component.begin(), component.end(),
                  [](EdgeId a, EdgeId b) { return a.value < b.value; });
        SemanticBoundaryRequest request;
        request.id = name + ":" + std::to_string(component.front().value);
        request.sourceEdges = component.size();
        const auto endpoint = std::find_if(incident.begin(), incident.end(),
                                           [](const auto& item) { return item.second.size() == 1; });
        const bool open = endpoint != incident.end();
        const bool degreesValid = std::all_of(
            incident.begin(), incident.end(), [open](const auto& item) {
                return item.second.size() == 2 || (open && item.second.size() == 1);
            });
        const std::size_t endpoints = static_cast<std::size_t>(std::count_if(
            incident.begin(), incident.end(), [](const auto& item) { return item.second.size() == 1; }));
        if (!degreesValid || (open && endpoints != 2)) {
            request.rejectionReason = "semantic boundary component branches";
            out.push_back(std::move(request));
            continue;
        }
        VertexId current{open ? endpoint->first : incident.begin()->first};
        EdgeId previous{};
        request.guide.mode = GuideMode::Topology;
        request.guide.closed = !open;
        request.guide.points.push_back(mesh.position(current));
        for (std::size_t step = 0; step < component.size(); ++step) {
            const auto& choices = incident[current.value];
            const auto nextIt = std::find_if(choices.begin(), choices.end(),
                                             [previous](EdgeId edge) { return edge != previous; });
            if (nextIt == choices.end()) {
                request.rejectionReason = "semantic boundary traversal stopped early";
                request.guide.points.clear();
                break;
            }
            const EdgeId next = *nextIt;
            const auto [a, b] = mesh.edgeVertices(next);
            const VertexId following = a == current ? b : a;
            request.guide.points.push_back(mesh.position(following));
            previous = next;
            current = following;
            if (request.guide.closed && current == VertexId{incident.begin()->first}) {
                break;
            }
        }
        if (request.guide.closed && request.guide.points.size() > 1) {
            // FlowGuide closes by wrapping its final segment to point zero. Do
            // not store point zero twice: the duplicate would manufacture a
            // degenerate segment for every consumer of this curve.
            request.guide.points.pop_back();
        }
        const std::size_t expectedPoints =
            request.guide.closed ? component.size() : component.size() + 1;
        if (request.rejectionReason.empty() && request.guide.points.size() != expectedPoints) {
            request.rejectionReason = "semantic boundary traversal did not cover its component";
            request.guide.points.clear();
        }
        out.push_back(std::move(request));
    }
    return out;
}

bool edgeAlignedToGuide(const Mesh& mesh, EdgeId edge, const FlowGuide& guide, float tolerance,
                        float cosLimit) {
    const auto [a, b] = mesh.edgeVertices(edge);
    const Vec3 pa = mesh.position(a);
    const Vec3 pb = mesh.position(b);
    const Vec3 midpoint = (pa + pb) * 0.5f;
    const Vec3 direction = normalized(pb - pa);
    const std::size_t segments = guide.closed ? guide.points.size() : guide.points.size() - 1;
    for (std::size_t i = 0; i < segments; ++i) {
        const Vec3 first = guide.points[i];
        const Vec3 second = guide.points[(i + 1) % guide.points.size()];
        const Vec3 segment = second - first;
        const float len2 = dot(segment, segment);
        const float t = len2 > 1e-20f ? std::clamp(dot(midpoint - first, segment) / len2, 0.0f, 1.0f)
                                      : 0.0f;
        if (length(midpoint - (first + segment * t)) <= tolerance &&
            std::abs(dot(direction, normalized(segment))) >= cosLimit) {
            return true;
        }
    }
    return false;
}

bool hasClosedAlignedComponent(const Mesh& mesh, const FlowGuide& guide, float tolerance,
                               float maxAngleDegrees) {
    const float cosLimit = std::cos(maxAngleDegrees * 3.14159265358979323846f / 180.0f);
    std::map<Index, std::vector<Index>> graph;
    for (Index edge = 0; edge < mesh.edgeCapacity(); ++edge) {
        const EdgeId id{edge};
        if (!mesh.isAlive(id) || !edgeAlignedToGuide(mesh, id, guide, tolerance, cosLimit)) {
            continue;
        }
        const auto [a, b] = mesh.edgeVertices(id);
        graph[a.value].push_back(b.value);
        graph[b.value].push_back(a.value);
    }
    std::map<Index, bool> visited;
    for (const auto& [seed, _] : graph) {
        if (visited[seed]) {
            continue;
        }
        bool closed = true;
        std::queue<Index> pending;
        pending.push(seed);
        visited[seed] = true;
        while (!pending.empty()) {
            const Index vertex = pending.front();
            pending.pop();
            closed = closed && graph[vertex].size() == 2;
            for (const Index next : graph[vertex]) {
                if (!visited[next]) {
                    visited[next] = true;
                    pending.push(next);
                }
            }
        }
        if (closed) {
            return true;
        }
    }
    return false;
}

}  // namespace

GuidePath projectGuideToPath(const Mesh& mesh, const FlowGuide& guide) {
    GuidePath out;
    if (guide.points.size() < 2 || mesh.vertexCapacity() == 0) {
        return out;
    }
    // Snap every guide point, then join consecutive snaps. Joining rather than
    // using the snaps alone is what makes the result a CONNECTED path: a guide
    // sampled more coarsely than the mesh would otherwise skip vertices and
    // leave gaps no edge chain can follow.
    std::vector<VertexId> anchors;
    for (const Vec3& p : guide.points) {
        const VertexId v = nearestVertex(mesh, p);
        if (!v.valid()) {
            return {};
        }
        if (anchors.empty() || anchors.back() != v) {
            anchors.push_back(v);
        }
    }
    if (guide.closed && anchors.size() > 2 && anchors.front() != anchors.back()) {
        anchors.push_back(anchors.front());
    }
    if (anchors.size() < 2) {
        return out;
    }

    for (std::size_t i = 0; i + 1 < anchors.size(); ++i) {
        const std::vector<VertexId> leg = shortestPath(mesh, anchors[i], anchors[i + 1]);
        if (leg.empty()) {
            return {};  // disconnected: report nothing rather than a broken path
        }
        for (std::size_t k = (out.vertices.empty() ? 0 : 1); k < leg.size(); ++k) {
            out.vertices.push_back(leg[k]);
        }
    }
    if (out.vertices.size() < 2) {
        return {};
    }
    for (std::size_t i = 0; i + 1 < out.vertices.size(); ++i) {
        const EdgeId e = mesh.edgeBetween(out.vertices[i], out.vertices[i + 1]);
        if (e.valid()) {
            out.edges.push_back(e);
        }
    }
    out.closed = guide.closed && out.vertices.front() == out.vertices.back();
    for (const VertexId v : out.vertices) {
        out.maxDeviation = std::max(out.maxDeviation,
                                    distanceToGuide(guide.points, guide.closed, mesh.position(v)));
    }
    return out;
}

void insertGuideArcs(TopologyLayout& layout, const GuidePath& path) {
    if (path.vertices.size() < 2) {
        return;
    }
    const auto firstNode = static_cast<LayoutNodeId>(layout.nodes.size());
    // A closed path repeats its first vertex at the end; that repeat becomes the
    // arc back to the first node rather than a duplicate node.
    const std::size_t nodeCount = path.closed ? path.vertices.size() - 1 : path.vertices.size();
    for (std::size_t i = 0; i < nodeCount; ++i) {
        LayoutNode n;
        n.id = static_cast<LayoutNodeId>(layout.nodes.size());
        n.kind = LayoutNodeKind::GuideAnchor;
        n.vertex = path.vertices[i];
        n.locked = true;  // an artist put this here; nothing may relocate it
        layout.nodes.push_back(n);
    }
    for (std::size_t i = 0; i < nodeCount; ++i) {
        const bool last = i + 1 == nodeCount;
        if (last && !path.closed) {
            break;
        }
        LayoutArc arc;
        arc.id = static_cast<LayoutArcId>(layout.arcs.size());
        arc.kind = LayoutArcKind::Guide;
        arc.begin = firstNode + static_cast<LayoutNodeId>(i);
        arc.end = firstNode + static_cast<LayoutNodeId>(last ? 0 : i + 1);
        arc.locked = true;
        layout.arcs.push_back(std::move(arc));
    }
}

GuideAdherence measureGuideAdherence(const Mesh& output, const FlowGuide& guide, float tolerance,
                                     float maxAngleDegrees) {
    GuideAdherence out;
    if (guide.points.size() < 2 || tolerance <= 0.0f) {
        return out;
    }
    // Sample the guide, and for each sample ask whether an output EDGE runs
    // through its neighbourhood — not whether a vertex is near it. A vertex
    // near the guide proves nothing; a continuous chain of edges along it is
    // what "the loop follows my stroke" means.
    constexpr int kSamplesPerSegment = 8;
    const std::size_t segments = guide.closed ? guide.points.size() : guide.points.size() - 1;
    std::vector<Vec3> samples;
    std::vector<Vec3> tangents;
    for (std::size_t i = 0; i < segments; ++i) {
        const Vec3 a = guide.points[i];
        const Vec3 b = guide.points[(i + 1) % guide.points.size()];
        const Vec3 tangent = normalized(b - a);
        for (int k = 0; k < kSamplesPerSegment; ++k) {
            const float t = static_cast<float>(k) / static_cast<float>(kSamplesPerSegment);
            samples.push_back(a + (b - a) * t);
            tangents.push_back(tangent);
        }
    }
    out.samples = samples.size();
    if (samples.empty()) {
        return out;
    }

    const float cosLimit = std::cos(maxAngleDegrees * 3.14159265358979323846f / 180.0f);
    std::size_t covered = 0;
    float sum = 0.0f;
    for (std::size_t i = 0; i < samples.size(); ++i) {
        const Vec3 s = samples[i];
        const Vec3 tangent = tangents[i];
        float best = kInf;         // nearest edge of any orientation, for reporting
        float bestAligned = kInf;  // nearest edge running ALONG the guide
        for (Index e = 0; e < output.edgeCapacity(); ++e) {
            const EdgeId edge{e};
            if (!output.isAlive(edge)) {
                continue;
            }
            const auto [a, b] = output.edgeVertices(edge);
            const Vec3 pa = output.position(a);
            const Vec3 ab = output.position(b) - pa;
            const float len2 = dot(ab, ab);
            const float t = len2 > 1e-20f ? std::clamp(dot(s - pa, ab) / len2, 0.0f, 1.0f) : 0.0f;
            const float d = length(s - (pa + ab * t));
            best = std::min(best, d);
            // Undirected: an edge running the other way along the guide is
            // still running along it.
            if (std::abs(dot(normalized(ab), tangent)) >= cosLimit) {
                bestAligned = std::min(bestAligned, d);
            }
        }
        if (!std::isfinite(best)) {
            continue;
        }
        sum += best;
        out.maxDistance = std::max(out.maxDistance, best);
        covered += bestAligned <= tolerance ? 1u : 0u;
    }
    out.meanDistance = sum / static_cast<float>(samples.size());
    out.edgeChainCoverage = static_cast<float>(covered) / static_cast<float>(samples.size());
    return out;
}

std::vector<SemanticBoundaryRequest> collectSemanticBoundaryRequests(const Mesh& mesh) {
    std::vector<SemanticBoundaryRequest> out;
    for (const char* name : {"group_id", "material_id"}) {
        const auto* values = mesh.faceAttributes().find<std::int32_t>(name);
        if (values == nullptr) {
            continue;
        }
        std::vector<SemanticBoundaryRequest> requests =
            collectAttributeBoundaries(mesh, name, *values);
        out.insert(out.end(), std::make_move_iterator(requests.begin()),
                   std::make_move_iterator(requests.end()));
    }
    return out;
}

SemanticBoundaryAdherence measureSemanticBoundaryAdherence(const Mesh& output,
                                                           const SemanticBoundaryRequest& request,
                                                           float tolerance,
                                                           float maxAngleDegrees) {
    SemanticBoundaryAdherence out;
    out.id = request.id;
    out.sourceEdges = request.sourceEdges;
    out.requestedClosed = request.guide.closed;
    if (!request.rejectionReason.empty()) {
        out.reason = request.rejectionReason;
        return out;
    }
    out.adherence = measureGuideAdherence(output, request.guide, tolerance, maxAngleDegrees);
    out.outputClosed = !out.requestedClosed ||
                       hasClosedAlignedComponent(output, request.guide, tolerance, maxAngleDegrees);
    out.realized = out.adherence.edgeChainCoverage >= 0.999f && out.outputClosed;
    if (!out.realized) {
        out.reason = out.outputClosed ? "output edge-chain coverage is incomplete"
                                      : "output edge chain does not close";
    }
    return out;
}

}  // namespace cyber::remesh
