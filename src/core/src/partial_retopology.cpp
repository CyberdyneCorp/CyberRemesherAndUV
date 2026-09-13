#include "cyber/core/partial_retopology.hpp"

#include <algorithm>
#include <limits>
#include <map>
#include <queue>

#include "cyber/core/bvh.hpp"

namespace cyber::remesh {
namespace {

PartialRetopologyAnalysis reject(std::string reason) {
    PartialRetopologyAnalysis result;
    result.reason = std::move(reason);
    return result;
}

Vec3 barycentric(Vec3 point, Vec3 a, Vec3 b, Vec3 c) {
    const Vec3 ab = b - a;
    const Vec3 ac = c - a;
    const Vec3 ap = point - a;
    const float d00 = dot(ab, ab);
    const float d01 = dot(ab, ac);
    const float d11 = dot(ac, ac);
    const float d20 = dot(ap, ab);
    const float d21 = dot(ap, ac);
    const float denominator = d00 * d11 - d01 * d01;
    if (denominator == 0.0f) {
        return {};
    }
    const float v = (d11 * d20 - d01 * d21) / denominator;
    const float w = (d00 * d21 - d01 * d20) / denominator;
    return {1.0f - v - w, v, w};
}

SourceCorrespondence findSourceCorrespondence(const Mesh& source,
                                              const std::vector<FaceId>& selectedFaces,
                                              VertexId outputVertex, Vec3 point) {
    SourceCorrespondence result;
    result.outputVertex = outputVertex;
    float bestDistanceSquared = std::numeric_limits<float>::max();
    for (const FaceId face : selectedFaces) {
        const std::vector<VertexId> vertices = source.faceVertices(face);
        for (std::size_t i = 2; i < vertices.size(); ++i) {
            const std::array<VertexId, 3> triangle = {vertices[0], vertices[i - 1], vertices[i]};
            const Vec3 closest =
                closestPointOnTriangle(point, source.position(triangle[0]),
                                       source.position(triangle[1]), source.position(triangle[2]));
            const float distanceSquared = lengthSquared(point - closest);
            if (distanceSquared < bestDistanceSquared ||
                (distanceSquared == bestDistanceSquared && face.value < result.sourceFace.value)) {
                bestDistanceSquared = distanceSquared;
                result.sourceFace = face;
                result.sourceTriangle = triangle;
                result.barycentric =
                    barycentric(closest, source.position(triangle[0]), source.position(triangle[1]),
                                source.position(triangle[2]));
            }
        }
    }
    result.distance = std::sqrt(bestDistanceSquared);
    result.confidence = bestDistanceSquared == 0.0f ? 1.0f : 0.0f;
    return result;
}

}  // namespace

PartialRetopologyAnalysis analyzePartialRetopology(const Mesh& source,
                                                   const PartialRetopologyRequest& request) {
    if (request.faces.empty()) {
        return reject("selected face set is empty");
    }

    std::vector<bool> selected(source.faceCapacity(), false);
    for (const FaceId face : request.faces) {
        if (!source.isAlive(face)) {
            return reject("selected face is not alive");
        }
        if (selected[face.value]) {
            return reject("selected face is duplicated");
        }
        selected[face.value] = true;
    }

    std::vector<FaceId> ordered = request.faces;
    std::sort(ordered.begin(), ordered.end(), [](FaceId a, FaceId b) { return a.value < b.value; });
    std::queue<FaceId> pending;
    std::vector<bool> visited(source.faceCapacity(), false);
    pending.push(ordered.front());
    visited[ordered.front().value] = true;
    std::size_t connected = 0;
    while (!pending.empty()) {
        const FaceId face = pending.front();
        pending.pop();
        ++connected;
        const std::vector<VertexId> vertices = source.faceVertices(face);
        for (std::size_t i = 0; i < vertices.size(); ++i) {
            const EdgeId edge =
                source.edgeBetween(vertices[i], vertices[(i + 1) % vertices.size()]);
            for (const FaceId neighbor : source.edgeFaces(edge)) {
                if (selected[neighbor.value] && !visited[neighbor.value]) {
                    visited[neighbor.value] = true;
                    pending.push(neighbor);
                }
            }
        }
    }
    if (connected != ordered.size()) {
        return reject("selected faces are disconnected");
    }

    std::map<Index, VertexId> next;
    std::map<Index, std::size_t> incoming;
    for (const FaceId face : ordered) {
        const std::vector<VertexId> vertices = source.faceVertices(face);
        for (std::size_t i = 0; i < vertices.size(); ++i) {
            const VertexId from = vertices[i];
            const VertexId to = vertices[(i + 1) % vertices.size()];
            const EdgeId edge = source.edgeBetween(from, to);
            const std::vector<FaceId> adjacent = source.edgeFaces(edge);
            if (adjacent.size() != 2) {
                return reject("selected region touches an open or non-manifold edge");
            }
            const std::size_t inside = static_cast<std::size_t>(selected[adjacent[0].value]) +
                                       static_cast<std::size_t>(selected[adjacent[1].value]);
            if (inside == 1) {
                if (!next.emplace(from.value, to).second || ++incoming[to.value] != 1) {
                    return reject("selected region boundary branches");
                }
            }
        }
    }
    if (next.empty()) {
        return reject("selected region has no exterior boundary");
    }
    if (next.size() != incoming.size()) {
        return reject("selected region boundary is not a simple loop");
    }

    const Index start = next.begin()->first;
    PartialRetopologyAnalysis result;
    Index current = start;
    do {
        const auto it = next.find(current);
        if (it == next.end()) {
            return reject("selected region boundary is not closed");
        }
        result.boundary.push_back(VertexId{current});
        current = it->second.value;
    } while (current != start && result.boundary.size() <= next.size());
    if (current != start || result.boundary.size() != next.size()) {
        return reject("selected region has multiple boundary loops");
    }
    if (request.allQuadsRequired && result.boundary.size() != 4) {
        return reject("exact all-quad partial retopology currently requires a four-edge boundary");
    }
    result.status = PartialRetopologyStatus::Ready;
    result.selectedFaces = std::move(ordered);
    return result;
}

PartialRetopologyResult partialRetopologize(const Mesh& source,
                                            const PartialRetopologyRequest& request) {
    PartialRetopologyResult result;
    result.mesh = source;
    result.analysis = analyzePartialRetopology(source, request);
    if (result.analysis.status != PartialRetopologyStatus::Ready) {
        result.reason = result.analysis.reason;
        return result;
    }

    const std::vector<VertexId>& boundary = result.analysis.boundary;
    Vec3 center;
    for (const VertexId vertex : boundary) {
        center += source.position(vertex);
    }
    center = center / static_cast<float>(boundary.size());

    for (const FaceId face : result.analysis.selectedFaces) {
        result.mesh.removeFace(face);
    }

    std::vector<VertexId> inner;
    inner.reserve(boundary.size());
    for (const VertexId vertex : boundary) {
        inner.push_back(result.mesh.addVertex(lerp(source.position(vertex), center, 0.5f)));
        result.correspondences.push_back(
            findSourceCorrespondence(source, result.analysis.selectedFaces, inner.back(),
                                     result.mesh.position(inner.back())));
    }
    for (std::size_t i = 0; i < boundary.size(); ++i) {
        const std::vector<VertexId> quad = {boundary[i], boundary[(i + 1) % boundary.size()],
                                            inner[(i + 1) % inner.size()], inner[i]};
        if (!result.mesh.addFace(quad).valid()) {
            result.mesh = source;
            result.reason = "failed to stitch the replacement boundary";
            return result;
        }
    }
    if (!result.mesh.addFace(inner).valid()) {
        result.mesh = source;
        result.reason = "failed to create the replacement center quad";
        return result;
    }
    if (!result.mesh.validate().empty()) {
        result.mesh = source;
        result.reason = "replacement violated mesh structural invariants";
        return result;
    }

    result.status = PartialRetopologyStatus::Applied;
    return result;
}

}  // namespace cyber::remesh
