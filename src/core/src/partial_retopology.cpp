#include "cyber/core/partial_retopology.hpp"

#include <algorithm>
#include <map>
#include <queue>

namespace cyber::remesh {
namespace {

PartialRetopologyAnalysis reject(std::string reason) {
    PartialRetopologyAnalysis result;
    result.reason = std::move(reason);
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

}  // namespace cyber::remesh
