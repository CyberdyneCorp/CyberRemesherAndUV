#include "cyber/retopo/actions.hpp"

#include <array>
#include <unordered_map>

namespace cyber::retopo {

VertexId addSnappedVertex(Mesh& mesh, Vec3 position, const SurfaceSnapper* snap) {
    const Vec3 p = (snap != nullptr && !snap->empty()) ? snap->snapToSurface(position).point : position;
    return mesh.addVertex(p);
}

FaceId createFace(Mesh& mesh, std::span<const Vec3> points, const SurfaceSnapper* snap) {
    if (points.size() < 3 || points.size() > 4) {
        return FaceId{};
    }
    std::vector<VertexId> verts;
    verts.reserve(points.size());
    for (const Vec3 p : points) {
        verts.push_back(addSnappedVertex(mesh, p, snap));
    }
    return mesh.addFace(verts);
}

FaceId extrudeEdge(Mesh& mesh, EdgeId boundary, Vec3 farA, Vec3 farB, const SurfaceSnapper* snap) {
    if (!mesh.isAlive(boundary)) {
        return FaceId{};
    }
    const auto [a, b] = mesh.edgeVertices(boundary);
    const VertexId wa = addSnappedVertex(mesh, farA, snap);
    const VertexId wb = addSnappedVertex(mesh, farB, snap);
    const std::array<VertexId, 4> quad = {a, b, wb, wa};
    return mesh.addFace(quad);
}

std::vector<FaceId> extrudeBoundary(Mesh& mesh, std::span<const EdgeId> boundary, Vec3 offset,
                                    const SurfaceSnapper* snap) {
    std::vector<FaceId> faces;
    std::unordered_map<Index, VertexId> lifted;
    auto lift = [&](VertexId v) {
        const auto it = lifted.find(v.value);
        if (it != lifted.end()) {
            return it->second;
        }
        const VertexId nv = addSnappedVertex(mesh, mesh.position(v) + offset, snap);
        lifted.emplace(v.value, nv);
        return nv;
    };
    for (const EdgeId e : boundary) {
        if (!mesh.isAlive(e)) {
            continue;
        }
        const auto [a, b] = mesh.edgeVertices(e);
        const VertexId wa = lift(a);
        const VertexId wb = lift(b);
        const std::array<VertexId, 4> quad = {a, b, wb, wa};
        const FaceId f = mesh.addFace(quad);
        if (f.valid()) {
            faces.push_back(f);
        }
    }
    return faces;
}

std::vector<FaceId> bridgeLoops(Mesh& mesh, std::span<const VertexId> loopA,
                                std::span<const VertexId> loopB) {
    std::vector<FaceId> faces;
    if (loopA.size() != loopB.size() || loopA.size() < 2) {
        return faces;
    }
    for (std::size_t i = 0; i + 1 < loopA.size(); ++i) {
        const std::array<VertexId, 4> quad = {loopA[i], loopA[i + 1], loopB[i + 1], loopB[i]};
        const FaceId f = mesh.addFace(quad);
        if (f.valid()) {
            faces.push_back(f);
        }
    }
    return faces;
}

namespace {

// Returns the edge opposite `across` in a quad, using its loop cycle. Invalid
// if the face is not a quad or does not contain the edge.
EdgeId oppositeQuadEdge(const Mesh& mesh, FaceId quad, EdgeId across) {
    const std::vector<VertexId> vs = mesh.faceVertices(quad);
    if (vs.size() != 4) {
        return EdgeId{};
    }
    for (std::size_t i = 0; i < 4; ++i) {
        const EdgeId e = mesh.edgeBetween(vs[i], vs[(i + 1) % 4]);
        if (e == across) {
            return mesh.edgeBetween(vs[(i + 2) % 4], vs[(i + 3) % 4]);
        }
    }
    return EdgeId{};
}

}  // namespace

FaceId insertLoop(Mesh& mesh, FaceId quad, EdgeId across) {
    if (!mesh.isAlive(quad) || !mesh.isAlive(across)) {
        return FaceId{};
    }
    const EdgeId opposite = oppositeQuadEdge(mesh, quad, across);
    if (!opposite.valid()) {
        return FaceId{};
    }
    const VertexId mid0 = mesh.splitEdge(across, 0.5f);
    const VertexId mid1 = mesh.splitEdge(opposite, 0.5f);
    if (!mid0.valid() || !mid1.valid()) {
        return FaceId{};
    }
    return mesh.splitFace(quad, mid0, mid1);
}

void deleteFaces(Mesh& mesh, std::span<const FaceId> faces) {
    for (const FaceId f : faces) {
        if (mesh.isAlive(f)) {
            mesh.removeFace(f);
        }
    }
}

bool mergePair(Mesh& mesh, EdgeId edge) {
    if (!mesh.isAlive(edge)) {
        return false;
    }
    return mesh.collapseEdge(edge);
}

bool rotateEdge(Mesh& mesh, EdgeId edge) {
    if (!mesh.isAlive(edge)) {
        return false;
    }
    return mesh.flipEdge(edge);
}

std::vector<FaceId> extrudeCylinder(Mesh& mesh, std::span<const Vec3> ring, Vec3 direction,
                                    const SurfaceSnapper* snap) {
    std::vector<FaceId> faces;
    const std::size_t n = ring.size();
    if (n < 3) {
        return faces;
    }
    std::vector<VertexId> bottom;
    std::vector<VertexId> top;
    bottom.reserve(n);
    top.reserve(n);
    for (const Vec3 p : ring) {
        bottom.push_back(addSnappedVertex(mesh, p, snap));
        top.push_back(addSnappedVertex(mesh, p + direction, snap));
    }
    for (std::size_t i = 0; i < n; ++i) {
        const std::size_t j = (i + 1) % n;
        const std::array<VertexId, 4> quad = {bottom[i], bottom[j], top[j], top[i]};
        const FaceId f = mesh.addFace(quad);
        if (f.valid()) {
            faces.push_back(f);
        }
    }
    return faces;
}

}  // namespace cyber::retopo
