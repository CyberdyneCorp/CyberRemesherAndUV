#pragma once

#include <cstdint>
#include <optional>
#include <span>
#include <string>
#include <vector>

#include "cyber/core/attributes.hpp"
#include "cyber/core/math.hpp"

namespace cyber {

using Index = std::uint32_t;
inline constexpr Index kInvalidIndex = 0xFFFFFFFFu;

struct VertexId {
    Index value = kInvalidIndex;
    [[nodiscard]] bool valid() const { return value != kInvalidIndex; }
    friend bool operator==(VertexId a, VertexId b) { return a.value == b.value; }
};
struct EdgeId {
    Index value = kInvalidIndex;
    [[nodiscard]] bool valid() const { return value != kInvalidIndex; }
    friend bool operator==(EdgeId a, EdgeId b) { return a.value == b.value; }
};
struct FaceId {
    Index value = kInvalidIndex;
    [[nodiscard]] bool valid() const { return value != kInvalidIndex; }
    friend bool operator==(FaceId a, FaceId b) { return a.value == b.value; }
};
struct LoopId {
    Index value = kInvalidIndex;
    [[nodiscard]] bool valid() const { return value != kInvalidIndex; }
    friend bool operator==(LoopId a, LoopId b) { return a.value == b.value; }
};

// Radial-edge half-edge mesh (BMesh-style) supporting triangles, quads and
// n-gons, including non-manifold input: an edge keeps a radial cycle of
// loops, so any number of faces may share it without data loss (mesh-core
// spec, "Non-manifold representation without data loss").
//
// Elements live in index-addressed arrays with alive flags and free lists;
// ids stay stable across unrelated mutations. All iteration orders and
// results are deterministic (mesh-core spec, "Deterministic operations").
class Mesh {
public:
    // ---- construction -------------------------------------------------
    VertexId addVertex(Vec3 position);
    // Creates a face over existing vertices (>= 3, no repeats); shared edges
    // are found or created and the face is appended to their radial cycles.
    // Returns invalid id (and changes nothing) on degenerate input.
    FaceId addFace(std::span<const VertexId> vertices);
    void removeFace(FaceId face);
    // Removes a vertex; only valid for vertices with no incident edges.
    bool removeIsolatedVertex(VertexId vertex);

    static Mesh fromIndexed(std::span<const Vec3> positions,
                            std::span<const std::vector<Index>> faces);
    // Compact, deterministic export: vertices ordered by id, faces by id,
    // each face starting at its stored first loop.
    void toIndexed(std::vector<Vec3>& positions, std::vector<std::vector<Index>>& faces) const;

    // ---- mutating operators (mesh-core spec, "Structural invariants") --
    // Merges `remove` into `keep`: incident edges/loops are rewritten,
    // duplicate edges merge their radial cycles, faces that degenerate
    // (< 3 distinct vertices) are deleted. Fails if the two vertices are
    // identical.
    bool mergeVertices(VertexId keep, VertexId remove);
    // Collapses the edge into its first vertex (placed at the midpoint by
    // default); incident faces degenerate and are removed.
    bool collapseEdge(EdgeId edge, bool placeAtMidpoint = true);
    // Flips the shared edge of exactly two triangles. Corner attributes of
    // the two rewritten faces reset to defaults (documented policy); vertex,
    // edge and face attributes are preserved.
    bool flipEdge(EdgeId edge);
    // Splits the edge at parameter t, inserting one vertex into the edge and
    // into every incident face's loop (face arity grows by one). Edge
    // attributes and the feature flag propagate to both halves; the new
    // vertex and new corners interpolate.
    VertexId splitEdge(EdgeId edge, float t = 0.5f);
    // Connects two non-adjacent corners of one face, splitting it in two.
    // Corner attributes are preserved on surviving loops and copied for the
    // two new loops. Returns the new face (the piece starting at `b`) or an
    // invalid id on failure.
    FaceId splitFace(FaceId face, VertexId a, VertexId b);
    void triangulateFace(FaceId face);
    void triangulate();
    // Linear (Catmull-Clark topology, no smoothing) subdivision into quads.
    // Vertex/edge/face/corner attributes propagate per the documented
    // policy. Returns a new mesh.
    [[nodiscard]] Mesh linearSubdivide() const;

    // ---- adjacency queries --------------------------------------------
    [[nodiscard]] std::size_t vertexCount() const { return m_aliveVertices; }
    [[nodiscard]] std::size_t edgeCount() const { return m_aliveEdges; }
    [[nodiscard]] std::size_t faceCount() const { return m_aliveFaces; }
    [[nodiscard]] std::size_t vertexCapacity() const { return m_vertices.size(); }
    [[nodiscard]] std::size_t edgeCapacity() const { return m_edges.size(); }
    [[nodiscard]] std::size_t faceCapacity() const { return m_faces.size(); }
    [[nodiscard]] std::size_t loopCapacity() const { return m_loops.size(); }

    [[nodiscard]] bool isAlive(VertexId v) const {
        return v.value < m_vertices.size() && m_vertices[v.value].alive;
    }
    [[nodiscard]] bool isAlive(EdgeId e) const {
        return e.value < m_edges.size() && m_edges[e.value].alive;
    }
    [[nodiscard]] bool isAlive(FaceId f) const {
        return f.value < m_faces.size() && m_faces[f.value].alive;
    }

    [[nodiscard]] Vec3 position(VertexId v) const { return m_vertices[v.value].position; }
    void setPosition(VertexId v, Vec3 p) { m_vertices[v.value].position = p; }

    [[nodiscard]] std::vector<VertexId> faceVertices(FaceId face) const;
    [[nodiscard]] std::vector<LoopId> faceLoops(FaceId face) const;
    [[nodiscard]] std::size_t faceSize(FaceId face) const { return m_faces[face.value].size; }
    [[nodiscard]] std::span<const EdgeId> vertexEdges(VertexId v) const {
        return m_vertices[v.value].edges;
    }
    [[nodiscard]] EdgeId edgeBetween(VertexId a, VertexId b) const;
    [[nodiscard]] std::pair<VertexId, VertexId> edgeVertices(EdgeId e) const {
        return {m_edges[e.value].v0, m_edges[e.value].v1};
    }
    // Faces around an edge in radial order (deterministic).
    [[nodiscard]] std::vector<FaceId> edgeFaces(EdgeId e) const;
    [[nodiscard]] std::size_t edgeFaceCount(EdgeId e) const;
    [[nodiscard]] bool isBoundaryEdge(EdgeId e) const { return edgeFaceCount(e) == 1; }
    [[nodiscard]] std::vector<FaceId> vertexFaces(VertexId v) const;

    [[nodiscard]] VertexId loopVertex(LoopId l) const { return m_loops[l.value].vertex; }
    [[nodiscard]] LoopId loopNext(LoopId l) const { return m_loops[l.value].next; }
    [[nodiscard]] FaceId loopFace(LoopId l) const { return m_loops[l.value].face; }

    // Face normal (Newell's method, robust for n-gons).
    [[nodiscard]] Vec3 faceNormal(FaceId face) const;
    [[nodiscard]] Vec3 faceCentroid(FaceId face) const;

    // ---- feature edges (mesh-core spec, "Feature edge tagging") --------
    // Tags edges whose dihedral angle exceeds the threshold, plus boundary
    // edges. Non-manifold (3+ face) edges are always tagged.
    void tagFeatureEdges(float dihedralAngleDegrees);
    [[nodiscard]] bool isFeatureEdge(EdgeId e) const { return m_edges[e.value].feature; }
    void setFeatureEdge(EdgeId e, bool feature) { m_edges[e.value].feature = feature; }

    // ---- islands (mesh-core spec, non-manifold-safe) --------------------
    // Face-connected components over shared edges. Every alive face appears
    // in exactly one island; islands and their faces are ordered by index.
    [[nodiscard]] std::vector<std::vector<FaceId>> islands() const;

    // ---- attributes -----------------------------------------------------
    AttributeSet& vertexAttributes() { return m_vertexAttrs; }
    AttributeSet& edgeAttributes() { return m_edgeAttrs; }
    AttributeSet& faceAttributes() { return m_faceAttrs; }
    AttributeSet& cornerAttributes() { return m_cornerAttrs; }
    [[nodiscard]] const AttributeSet& vertexAttributes() const { return m_vertexAttrs; }
    [[nodiscard]] const AttributeSet& edgeAttributes() const { return m_edgeAttrs; }
    [[nodiscard]] const AttributeSet& faceAttributes() const { return m_faceAttrs; }
    [[nodiscard]] const AttributeSet& cornerAttributes() const { return m_cornerAttrs; }

    // ---- validation (mesh-core spec, "Structural invariants") ----------
    // Returns human-readable descriptions of every violated invariant;
    // empty means the structure is consistent.
    [[nodiscard]] std::vector<std::string> validate() const;

private:
    struct Vertex {
        Vec3 position;
        std::vector<EdgeId> edges;  // incident edges, insertion-ordered
        bool alive = false;
    };
    struct Edge {
        VertexId v0, v1;
        LoopId radial;  // one loop of the radial cycle (invalid if none)
        bool feature = false;
        bool alive = false;
    };
    struct Loop {
        VertexId vertex;  // corner vertex (origin within its face cycle)
        EdgeId edge;      // edge toward next corner
        FaceId face;
        LoopId next, prev;              // face cycle
        LoopId radialNext, radialPrev;  // radial cycle around `edge`
        bool alive = false;
    };
    struct Face {
        LoopId first;
        Index size = 0;
        bool alive = false;
    };

    VertexId allocVertex();
    EdgeId allocEdge();
    FaceId allocFace();
    LoopId allocLoop();
    void freeVertex(VertexId v);
    void freeEdge(EdgeId e);
    void freeFace(FaceId f);
    void freeLoop(LoopId l);

    EdgeId findOrCreateEdge(VertexId a, VertexId b);
    void radialInsert(EdgeId e, LoopId l);
    void radialRemove(LoopId l);
    void vertexEdgeListRemove(VertexId v, EdgeId e);
    void destroyEdgeIfUnused(EdgeId e);

    std::vector<Vertex> m_vertices;
    std::vector<Edge> m_edges;
    std::vector<Loop> m_loops;
    std::vector<Face> m_faces;
    std::vector<Index> m_freeVertices, m_freeEdges, m_freeLoops, m_freeFaces;
    std::size_t m_aliveVertices = 0, m_aliveEdges = 0, m_aliveFaces = 0, m_aliveLoops = 0;

    AttributeSet m_vertexAttrs, m_edgeAttrs, m_faceAttrs, m_cornerAttrs;
};

}  // namespace cyber
