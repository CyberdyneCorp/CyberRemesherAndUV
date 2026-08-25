#include "cyber/retopo/loop_subdivide.hpp"

#include <array>
#include <cmath>
#include <optional>
#include <vector>

#include "cyber/core/attributes.hpp"
#include "cyber/core/math.hpp"

namespace cyber::retopo {
namespace {

using Row = AttributeSet::Row;

// A validated triangle: corners in loop order plus the edge that leaves each
// corner, so e[i] joins v[i] to v[(i+1)%3].
struct Triangle {
    std::array<VertexId, 3> v{};
    std::array<EdgeId, 3> e{};
    std::array<LoopId, 3> loop{};
};

// Reads a triangular face's corners and edges. The caller has already ruled out
// non-triangles; nothing comes back when an edge the face claims cannot be
// found, which is structural corruption and is refused rather than papered
// over.
[[nodiscard]] std::optional<Triangle> triangleOf(const Mesh& mesh, FaceId face) {
    const std::vector<LoopId> loops = mesh.faceLoops(face);
    if (loops.size() != 3) {
        return std::nullopt;  // faceSize() and the loop cycle disagree
    }
    Triangle tri;
    for (std::size_t i = 0; i < 3; ++i) {
        tri.loop[i] = loops[i];
        tri.v[i] = mesh.loopVertex(loops[i]);
    }
    for (std::size_t i = 0; i < 3; ++i) {
        tri.e[i] = mesh.edgeBetween(tri.v[i], tri.v[(i + 1) % 3]);
        if (!tri.e[i].valid()) {
            return std::nullopt;
        }
    }
    return tri;
}

// The vertex of a triangle that is not an endpoint of `edge` — the apex the
// interior edge mask weighs at 1/8. Invalid when `face` is not a triangle or
// does not actually contain the edge.
[[nodiscard]] VertexId apexAcross(const Mesh& mesh, FaceId face, VertexId a, VertexId b) {
    for (const VertexId v : mesh.faceVertices(face)) {
        if (!(v == a) && !(v == b)) {
            return v;
        }
    }
    return VertexId{};
}

// Loop's odd (edge) point. Interior edges use 3/8 on each endpoint and 1/8 on
// each of the two apexes; boundary edges use the plain midpoint, which is the
// cubic-B-spline curve rule for the boundary polyline and is what keeps an open
// mesh's border where the caller put it. Non-manifold edges (3+ faces) have no
// defined mask, so they fall back to the midpoint too.
[[nodiscard]] Vec3 loopEdgePoint(const Mesh& mesh, EdgeId edge) {
    const auto [a, b] = mesh.edgeVertices(edge);
    const Vec3 midpoint = lerp(mesh.position(a), mesh.position(b), 0.5f);

    const std::vector<FaceId> faces = mesh.edgeFaces(edge);
    if (faces.size() != 2) {
        return midpoint;
    }
    Vec3 apexSum{};
    for (const FaceId f : faces) {
        const VertexId apex = apexAcross(mesh, f, a, b);
        if (!apex.valid()) {
            return midpoint;
        }
        apexSum += mesh.position(apex);
    }
    return (mesh.position(a) + mesh.position(b)) * 0.375f + apexSum * 0.125f;
}

// Loop's original beta for a valence-n interior vertex:
//   beta = (1/n) * (5/8 - (3/8 + (1/4)cos(2*pi/n))^2)
// which is 1/16 at the regular valence 6.
[[nodiscard]] float loopBeta(std::size_t valence) {
    const float n = static_cast<float>(valence);
    const float inner = 0.375f + 0.25f * std::cos(2.0f * kPi / n);
    return (0.625f - inner * inner) / n;
}

// The boundary neighbours of `v`: the other endpoints of its incident boundary
// edges. A manifold boundary vertex has exactly two.
[[nodiscard]] std::vector<VertexId> boundaryNeighbors(const Mesh& mesh, VertexId v) {
    std::vector<VertexId> out;
    for (const EdgeId e : mesh.vertexEdges(v)) {
        if (mesh.isBoundaryEdge(e)) {
            const auto [a, b] = mesh.edgeVertices(e);
            out.push_back((a == v) ? b : a);
        }
    }
    return out;
}

// Loop's even (original) point. A vertex with any boundary edge is repositioned
// by the 1/8, 3/4, 1/8 curve rule along the boundary ONLY — applying the
// interior mask there would pull the border toward the surface interior and
// shrink an open mesh a little more at every level. Vertices whose boundary is
// not a clean two-edge run (a corner where two boundary loops pinch, or a
// non-manifold fan) are held exactly in place: there is no curve to smooth
// along, and moving them is how such spots unravel.
[[nodiscard]] Vec3 loopVertexPoint(const Mesh& mesh, VertexId v) {
    const std::vector<VertexId> onBoundary = boundaryNeighbors(mesh, v);
    if (!onBoundary.empty()) {
        if (onBoundary.size() != 2) {
            return mesh.position(v);
        }
        return mesh.position(v) * 0.75f +
               (mesh.position(onBoundary[0]) + mesh.position(onBoundary[1])) * 0.125f;
    }

    const std::span<const EdgeId> edges = mesh.vertexEdges(v);
    if (edges.size() < 3) {
        return mesh.position(v);  // no interior mask below valence 3
    }
    Vec3 ringSum{};
    for (const EdgeId e : edges) {
        const auto [a, b] = mesh.edgeVertices(e);
        ringSum += mesh.position((a == v) ? b : a);
    }
    const float beta = loopBeta(edges.size());
    return mesh.position(v) * (1.0f - static_cast<float>(edges.size()) * beta) + ringSum * beta;
}

// Builds the refined mesh. Holds the two id maps (original vertex -> new
// vertex, original edge -> its new edge point) so the emit pass reads as the
// four-triangle stencil it is.
class Refiner {
public:
    Refiner(const Mesh& source, LoopSubdivideMode mode) : m_src(source), m_mode(mode) {
        m_out.vertexAttributes().adoptSchema(source.vertexAttributes());
        m_out.edgeAttributes().adoptSchema(source.edgeAttributes());
        m_out.faceAttributes().adoptSchema(source.faceAttributes());
        m_out.cornerAttributes().adoptSchema(source.cornerAttributes());
    }

    void seedVertices() {
        m_vertexMap.assign(m_src.vertexCapacity(), VertexId{});
        for (Index i = 0; i < m_src.vertexCapacity(); ++i) {
            const VertexId v{i};
            if (!m_src.isAlive(v)) {
                continue;
            }
            const Vec3 p =
                m_mode == LoopSubdivideMode::Smooth ? loopVertexPoint(m_src, v) : m_src.position(v);
            m_vertexMap[i] = m_out.addVertex(p);
            m_out.vertexAttributes().applyRow(m_vertexMap[i].value,
                                              m_src.vertexAttributes().extractRow(i));
        }

        m_midMap.assign(m_src.edgeCapacity(), VertexId{});
        for (Index i = 0; i < m_src.edgeCapacity(); ++i) {
            const EdgeId e{i};
            if (!m_src.isAlive(e)) {
                continue;
            }
            const auto [a, b] = m_src.edgeVertices(e);
            const Vec3 p = m_mode == LoopSubdivideMode::Smooth
                               ? loopEdgePoint(m_src, e)
                               : lerp(m_src.position(a), m_src.position(b), 0.5f);
            m_midMap[i] = m_out.addVertex(p);
            m_out.vertexAttributes().applyRow(
                m_midMap[i].value,
                AttributeSet::averageRows({m_src.vertexAttributes().extractRow(a.value),
                                           m_src.vertexAttributes().extractRow(b.value)}));
        }
    }

    // The 1-to-4 stencil: three corner triangles and the inverted centre one.
    void emitFace(FaceId face, const Triangle& tri) {
        std::array<VertexId, 3> corner{};
        std::array<VertexId, 3> mid{};
        std::array<Row, 3> cornerRow{};
        for (std::size_t i = 0; i < 3; ++i) {
            corner[i] = m_vertexMap[tri.v[i].value];
            mid[i] = m_midMap[tri.e[i].value];
            cornerRow[i] = m_src.cornerAttributes().extractRow(tri.loop[i].value);
        }
        std::array<Row, 3> edgeRow{};
        for (std::size_t i = 0; i < 3; ++i) {
            edgeRow[i] = AttributeSet::averageRows({cornerRow[i], cornerRow[(i + 1) % 3]});
        }

        const Row faceRow = m_src.faceAttributes().extractRow(face.value);
        for (std::size_t i = 0; i < 3; ++i) {
            const std::size_t prev = (i + 2) % 3;
            addChild({corner[i], mid[i], mid[prev]}, faceRow,
                     {cornerRow[i], edgeRow[i], edgeRow[prev]});
        }
        addChild({mid[0], mid[1], mid[2]}, faceRow, {edgeRow[0], edgeRow[1], edgeRow[2]});

        propagateFeatures(tri, corner, mid);
    }

    [[nodiscard]] Mesh take() { return std::move(m_out); }

private:
    void addChild(std::array<VertexId, 3> verts, const Row& faceRow,
                  const std::array<Row, 3>& corners) {
        const FaceId child = m_out.addFace(verts);
        if (!child.valid()) {
            return;
        }
        m_out.faceAttributes().applyRow(child.value, faceRow);
        const std::vector<LoopId> loops = m_out.faceLoops(child);
        for (std::size_t i = 0; i < loops.size() && i < corners.size(); ++i) {
            m_out.cornerAttributes().applyRow(loops[i].value, corners[i]);
        }
    }

    // Both halves of a feature parent edge stay feature edges, so a tagged
    // crease survives densification instead of dissolving into the smooth run.
    void propagateFeatures(const Triangle& tri, const std::array<VertexId, 3>& corner,
                           const std::array<VertexId, 3>& mid) {
        for (std::size_t i = 0; i < 3; ++i) {
            if (!m_src.isFeatureEdge(tri.e[i])) {
                continue;
            }
            for (const auto& [x, y] :
                 {std::pair{corner[i], mid[i]}, std::pair{mid[i], corner[(i + 1) % 3]}}) {
                const EdgeId child = m_out.edgeBetween(x, y);
                if (child.valid()) {
                    m_out.setFeatureEdge(child, true);
                }
            }
        }
    }

    const Mesh& m_src;
    LoopSubdivideMode m_mode;
    Mesh m_out;
    std::vector<VertexId> m_vertexMap;
    std::vector<VertexId> m_midMap;
};

}  // namespace

const char* toString(LoopSubdivideError error) {
    switch (error) {
        case LoopSubdivideError::None:
            return "ok";
        case LoopSubdivideError::EmptyMesh:
            return "mesh has no faces";
        case LoopSubdivideError::NonTriangleFace:
            return "mesh has a face that is not a triangle";
        case LoopSubdivideError::MalformedFace:
            return "mesh has a triangle whose edges cannot be resolved";
    }
    return "unknown";
}

LoopSubdivideResult loopSubdivide(const Mesh& mesh, LoopSubdivideMode mode) {
    LoopSubdivideResult result;
    if (mesh.faceCount() == 0) {
        result.error = LoopSubdivideError::EmptyMesh;
        return result;
    }

    // Validate the WHOLE mesh before building anything: the caller must get
    // either a fully subdivided mesh or their input back, never a half-refined
    // one that stopped at the first quad.
    std::vector<Triangle> triangles(mesh.faceCapacity());
    for (Index i = 0; i < mesh.faceCapacity(); ++i) {
        const FaceId f{i};
        if (!mesh.isAlive(f)) {
            continue;
        }
        if (mesh.faceSize(f) != 3) {
            result.error = LoopSubdivideError::NonTriangleFace;
            result.offendingFace = f;
            result.offendingFaceSize = mesh.faceSize(f);
            return result;
        }
        const std::optional<Triangle> tri = triangleOf(mesh, f);
        if (!tri.has_value()) {
            result.error = LoopSubdivideError::MalformedFace;
            result.offendingFace = f;
            result.offendingFaceSize = mesh.faceSize(f);
            return result;
        }
        triangles[i] = *tri;
    }

    Refiner refiner(mesh, mode);
    refiner.seedVertices();
    for (Index i = 0; i < mesh.faceCapacity(); ++i) {
        const FaceId f{i};
        if (mesh.isAlive(f)) {
            refiner.emitFace(f, triangles[i]);
        }
    }
    result.mesh = refiner.take();
    return result;
}

}  // namespace cyber::retopo
