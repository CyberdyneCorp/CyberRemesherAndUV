#include "cyber/core/partial_retopology.hpp"

#include <algorithm>
#include <limits>
#include <map>
#include <queue>
#include <type_traits>

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

void transferVertexAttributes(const Mesh& source, Mesh& output,
                              const SourceCorrespondence& correspondence) {
    source.vertexAttributes().forEachColumnPaired(
        output.vertexAttributes(), [&](const auto& input, auto& result) {
            using T = typename std::decay_t<decltype(input)>::value_type;
            const std::array<VertexId, 3>& triangle = correspondence.sourceTriangle;
            const Vec3 weights = correspondence.barycentric;
            if constexpr (std::is_same_v<T, std::int32_t>) {
                std::size_t nearest = 0;
                if (weights.y > weights.x && weights.y >= weights.z) {
                    nearest = 1;
                } else if (weights.z > weights.x && weights.z > weights.y) {
                    nearest = 2;
                }
                result[correspondence.outputVertex.value] = input[triangle[nearest].value];
            } else {
                result[correspondence.outputVertex.value] = input[triangle[0].value] * weights.x +
                                                            input[triangle[1].value] * weights.y +
                                                            input[triangle[2].value] * weights.z;
            }
        });
}

void listUntransferredAttributes(const Mesh& source, PartialRetopologyResult& result) {
    const auto listDomain = [&](const char* domain, const AttributeSet& attributes) {
        attributes.forEachColumn([&](const std::string& name, const auto&) {
            result.untransferredAttributes.push_back(std::string(domain) + ":" + name);
        });
    };
    listDomain("edge", source.edgeAttributes());
    listDomain("corner", source.cornerAttributes());
}

void transferFaceAttributes(const Mesh& source, Mesh& output, FaceId outputFace,
                            FaceId sourceFace) {
    output.faceAttributes().applyRow(outputFace.value,
                                     source.faceAttributes().extractRow(sourceFace.value));
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
    result.sourceVertexToOutput.resize(source.vertexCapacity());
    for (Index vertex = 0; vertex < source.vertexCapacity(); ++vertex) {
        if (source.isAlive(VertexId{vertex})) {
            result.sourceVertexToOutput[vertex] = VertexId{vertex};
        }
    }
    result.analysis = analyzePartialRetopology(source, request);
    if (result.analysis.status != PartialRetopologyStatus::Ready) {
        result.reason = result.analysis.reason;
        return result;
    }
    listUntransferredAttributes(source, result);

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
        transferVertexAttributes(source, result.mesh, result.correspondences.back());
    }
    const FaceId attributeSource = result.analysis.selectedFaces.front();
    for (std::size_t i = 0; i < boundary.size(); ++i) {
        const std::vector<VertexId> quad = {boundary[i], boundary[(i + 1) % boundary.size()],
                                            inner[(i + 1) % inner.size()], inner[i]};
        const FaceId newFace = result.mesh.addFace(quad);
        if (!newFace.valid()) {
            result.mesh = source;
            result.reason = "failed to stitch the replacement boundary";
            return result;
        }
        transferFaceAttributes(source, result.mesh, newFace, attributeSource);
    }
    const FaceId centerFace = result.mesh.addFace(inner);
    if (!centerFace.valid()) {
        result.mesh = source;
        result.reason = "failed to create the replacement center quad";
        return result;
    }
    transferFaceAttributes(source, result.mesh, centerFace, attributeSource);
    if (!result.mesh.validate().empty()) {
        result.mesh = source;
        result.reason = "replacement violated mesh structural invariants";
        return result;
    }

    result.status = PartialRetopologyStatus::Applied;
    return result;
}

}  // namespace cyber::remesh
