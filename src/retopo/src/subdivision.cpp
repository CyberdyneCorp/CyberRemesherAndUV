#include "cyber/retopo/subdivision.hpp"

#include <cstddef>
#include <vector>

namespace cyber::retopo {

namespace {

// What counts as a crease. A tagged feature edge, or any edge not shared by
// exactly two faces: boundary (one face), non-manifold (three or more) and
// wire (none). This is the same predicate relaxQuadMesh() freezes vertices on
// in src/core/src/pipeline.cpp — the stages have to agree, otherwise a smooth
// pass rounds a crease off and the relax that follows pins the rounded result.
bool isCrease(const Mesh& mesh, EdgeId e) {
    return mesh.isFeatureEdge(e) || mesh.edgeFaceCount(e) != 2;
}

// Smooth vertex rule: (F + 2R + (n-3)P) / n, with F the average of the
// adjacent face points, R the average of the incident edge MIDPOINTS (not of
// the neighbours — the classic statement of the rule) and n the valence.
// Below valence 3 the (n-3) term goes negative and the vertex is not an
// interior B-spline vertex anyway, so it is left alone.
Vec3 smoothVertexPoint(const Mesh& mesh, VertexId v) {
    const std::span<const EdgeId> edges = mesh.vertexEdges(v);
    const std::vector<FaceId> faces = mesh.vertexFaces(v);
    const Vec3 p = mesh.position(v);
    if (edges.size() < 3 || faces.empty()) {
        return p;
    }
    Vec3 edgeSum{};
    for (const EdgeId e : edges) {
        const auto [a, b] = mesh.edgeVertices(e);
        edgeSum += lerp(mesh.position(a), mesh.position(b), 0.5f);
    }
    Vec3 faceSum{};
    for (const FaceId f : faces) {
        faceSum += mesh.faceCentroid(f);
    }
    const float n = static_cast<float>(edges.size());
    const Vec3 r = edgeSum / n;
    const Vec3 f = faceSum / static_cast<float>(faces.size());
    return (f + r * 2.0f + p * (n - 3.0f)) / n;
}

// Which of the three vertex rules applies, and the resulting position.
Vec3 vertexPoint(const Mesh& mesh, VertexId v) {
    const std::span<const EdgeId> edges = mesh.vertexEdges(v);
    const Vec3 p = mesh.position(v);
    Vec3 creaseNeighbours{};
    std::size_t creases = 0;
    for (const EdgeId e : edges) {
        if (!isCrease(mesh, e)) {
            continue;
        }
        if (++creases <= 2) {
            const auto [a, b] = mesh.edgeVertices(e);
            creaseNeighbours += mesh.position(a == v ? b : a);
        }
    }
    // Corner rule (position frozen): three or more creases meet, so there is no
    // single crease curve to follow; or the vertex has valence two and both its
    // edges are creases — the corner of an open patch, which the crease rule
    // would visibly round off ("keep corners", OpenSubdiv's edge-and-corner
    // boundary interpolation).
    if (creases > 2 || (creases == 2 && edges.size() == 2)) {
        return p;
    }
    // Crease rule: the cubic B-spline curve rule (1/8, 3/4, 1/8) along the two
    // crease edges, so a border or a tagged sharp edge keeps its shape instead
    // of being pulled toward the surface interior.
    if (creases == 2) {
        return (creaseNeighbours + p * 6.0f) * 0.125f;
    }
    // Zero creases (interior) or one (a dart, whose end is smooth by Hoppe's
    // rules).
    return smoothVertexPoint(mesh, v);
}

Mesh catmullClark(const Mesh& mesh) {
    Mesh::SubdivisionMap map;
    Mesh out = mesh.linearSubdivide(&map);

    // Face points need no work: the Catmull-Clark face point IS the centroid of
    // the face's vertices, which is where the linear pass already put it.

    for (Index ei = 0; ei < mesh.edgeCapacity(); ++ei) {
        const EdgeId e{ei};
        // A crease edge point stays at the midpoint (the sharp rule), which the
        // linear pass wrote, so creases are skipped rather than recomputed.
        if (!mesh.isAlive(e) || isCrease(mesh, e) || !map.edgePoints[ei].valid()) {
            continue;
        }
        const auto [a, b] = mesh.edgeVertices(e);
        const std::vector<FaceId> faces = mesh.edgeFaces(e);  // exactly two, or isCrease held
        const Vec3 sum = mesh.position(a) + mesh.position(b) + mesh.faceCentroid(faces[0]) +
                         mesh.faceCentroid(faces[1]);
        out.setPosition(map.edgePoints[ei], sum * 0.25f);
    }

    // Vertex points read the SOURCE mesh throughout — the rules are defined on
    // the coarse cage, so no vertex may see another's new position.
    for (Index vi = 0; vi < mesh.vertexCapacity(); ++vi) {
        const VertexId v{vi};
        if (!mesh.isAlive(v) || !map.vertexPoints[vi].valid()) {
            continue;
        }
        out.setPosition(map.vertexPoints[vi], vertexPoint(mesh, v));
    }
    return out;
}

}  // namespace

Mesh subdivide(const Mesh& mesh, SubdivisionMode mode) {
    return mode == SubdivisionMode::CatmullClark ? catmullClark(mesh) : mesh.linearSubdivide();
}

}  // namespace cyber::retopo
