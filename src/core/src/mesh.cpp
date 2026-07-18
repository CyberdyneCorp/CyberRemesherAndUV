#include "cyber/core/mesh.hpp"

#include <algorithm>
#include <cassert>

namespace cyber {

// ---- element allocation -------------------------------------------------

VertexId Mesh::allocVertex() {
    Index i;
    if (!m_freeVertices.empty()) {
        i = m_freeVertices.back();
        m_freeVertices.pop_back();
    } else {
        i = static_cast<Index>(m_vertices.size());
        m_vertices.emplace_back();
        m_vertexAttrs.resize(m_vertices.size());
    }
    m_vertices[i] = Vertex{};
    m_vertices[i].alive = true;
    ++m_aliveVertices;
    return {i};
}

EdgeId Mesh::allocEdge() {
    Index i;
    if (!m_freeEdges.empty()) {
        i = m_freeEdges.back();
        m_freeEdges.pop_back();
    } else {
        i = static_cast<Index>(m_edges.size());
        m_edges.emplace_back();
        m_edgeAttrs.resize(m_edges.size());
    }
    m_edges[i] = Edge{};
    m_edges[i].alive = true;
    ++m_aliveEdges;
    return {i};
}

FaceId Mesh::allocFace() {
    Index i;
    if (!m_freeFaces.empty()) {
        i = m_freeFaces.back();
        m_freeFaces.pop_back();
    } else {
        i = static_cast<Index>(m_faces.size());
        m_faces.emplace_back();
        m_faceAttrs.resize(m_faces.size());
    }
    m_faces[i] = Face{};
    m_faces[i].alive = true;
    ++m_aliveFaces;
    return {i};
}

LoopId Mesh::allocLoop() {
    Index i;
    if (!m_freeLoops.empty()) {
        i = m_freeLoops.back();
        m_freeLoops.pop_back();
    } else {
        i = static_cast<Index>(m_loops.size());
        m_loops.emplace_back();
        m_cornerAttrs.resize(m_loops.size());
    }
    m_loops[i] = Loop{};
    m_loops[i].alive = true;
    ++m_aliveLoops;
    return {i};
}

void Mesh::freeVertex(VertexId v) {
    assert(m_vertices[v.value].edges.empty());
    m_vertices[v.value].alive = false;
    m_freeVertices.push_back(v.value);
    --m_aliveVertices;
}

void Mesh::freeEdge(EdgeId e) {
    m_edges[e.value].alive = false;
    m_freeEdges.push_back(e.value);
    --m_aliveEdges;
}

void Mesh::freeFace(FaceId f) {
    m_faces[f.value].alive = false;
    m_freeFaces.push_back(f.value);
    --m_aliveFaces;
}

void Mesh::freeLoop(LoopId l) {
    m_loops[l.value].alive = false;
    m_freeLoops.push_back(l.value);
    --m_aliveLoops;
}

// ---- low-level linkage ----------------------------------------------------

EdgeId Mesh::findOrCreateEdge(VertexId a, VertexId b) {
    for (const EdgeId e : m_vertices[a.value].edges) {
        const Edge& edge = m_edges[e.value];
        if ((edge.v0 == a && edge.v1 == b) || (edge.v0 == b && edge.v1 == a)) {
            return e;
        }
    }
    const EdgeId e = allocEdge();
    m_edges[e.value].v0 = a;
    m_edges[e.value].v1 = b;
    m_vertices[a.value].edges.push_back(e);
    m_vertices[b.value].edges.push_back(e);
    return e;
}

void Mesh::radialInsert(EdgeId e, LoopId l) {
    Edge& edge = m_edges[e.value];
    Loop& loop = m_loops[l.value];
    loop.edge = e;
    if (!edge.radial.valid()) {
        loop.radialNext = l;
        loop.radialPrev = l;
        edge.radial = l;
        return;
    }
    const LoopId head = edge.radial;
    const LoopId tail = m_loops[head.value].radialPrev;
    loop.radialNext = head;
    loop.radialPrev = tail;
    m_loops[tail.value].radialNext = l;
    m_loops[head.value].radialPrev = l;
}

void Mesh::radialRemove(LoopId l) {
    Loop& loop = m_loops[l.value];
    Edge& edge = m_edges[loop.edge.value];
    if (loop.radialNext == l) {
        edge.radial = {};
    } else {
        m_loops[loop.radialPrev.value].radialNext = loop.radialNext;
        m_loops[loop.radialNext.value].radialPrev = loop.radialPrev;
        if (edge.radial == l) {
            edge.radial = loop.radialNext;
        }
    }
    loop.radialNext = {};
    loop.radialPrev = {};
}

void Mesh::vertexEdgeListRemove(VertexId v, EdgeId e) {
    auto& edges = m_vertices[v.value].edges;
    edges.erase(std::remove(edges.begin(), edges.end(), e), edges.end());
}

void Mesh::destroyEdgeIfUnused(EdgeId e) {
    Edge& edge = m_edges[e.value];
    if (!edge.alive || edge.radial.valid()) {
        return;
    }
    vertexEdgeListRemove(edge.v0, e);
    vertexEdgeListRemove(edge.v1, e);
    freeEdge(e);
}

// ---- construction ----------------------------------------------------------

VertexId Mesh::addVertex(Vec3 position) {
    const VertexId v = allocVertex();
    m_vertices[v.value].position = position;
    return v;
}

FaceId Mesh::addFace(std::span<const VertexId> vertices) {
    const std::size_t n = vertices.size();
    if (n < 3) {
        return {};
    }
    for (std::size_t i = 0; i < n; ++i) {
        if (!isAlive(vertices[i])) {
            return {};
        }
        for (std::size_t j = i + 1; j < n; ++j) {
            if (vertices[i] == vertices[j]) {
                return {};
            }
        }
    }

    const FaceId f = allocFace();
    std::vector<LoopId> loops(n);
    for (std::size_t i = 0; i < n; ++i) {
        loops[i] = allocLoop();
    }
    for (std::size_t i = 0; i < n; ++i) {
        Loop& loop = m_loops[loops[i].value];
        loop.vertex = vertices[i];
        loop.face = f;
        loop.next = loops[(i + 1) % n];
        loop.prev = loops[(i + n - 1) % n];
        const EdgeId e = findOrCreateEdge(vertices[i], vertices[(i + 1) % n]);
        radialInsert(e, loops[i]);
    }
    m_faces[f.value].first = loops[0];
    m_faces[f.value].size = static_cast<Index>(n);
    return f;
}

void Mesh::removeFace(FaceId face) {
    if (!isAlive(face)) {
        return;
    }
    const std::vector<LoopId> loops = faceLoops(face);
    std::vector<EdgeId> edges;
    edges.reserve(loops.size());
    for (const LoopId l : loops) {
        edges.push_back(m_loops[l.value].edge);
        radialRemove(l);
        freeLoop(l);
    }
    for (const EdgeId e : edges) {
        destroyEdgeIfUnused(e);
    }
    freeFace(face);
}

bool Mesh::removeIsolatedVertex(VertexId vertex) {
    if (!isAlive(vertex) || !m_vertices[vertex.value].edges.empty()) {
        return false;
    }
    freeVertex(vertex);
    return true;
}

Mesh Mesh::fromIndexed(std::span<const Vec3> positions, std::span<const std::vector<Index>> faces) {
    Mesh mesh;
    std::vector<VertexId> ids;
    ids.reserve(positions.size());
    for (const Vec3& p : positions) {
        ids.push_back(mesh.addVertex(p));
    }
    std::vector<VertexId> faceIds;
    for (const auto& face : faces) {
        faceIds.clear();
        bool ok = true;
        for (const Index i : face) {
            if (i >= ids.size()) {
                ok = false;
                break;
            }
            faceIds.push_back(ids[i]);
        }
        if (ok) {
            mesh.addFace(faceIds);
        }
    }
    return mesh;
}

void Mesh::toIndexed(std::vector<Vec3>& positions, std::vector<std::vector<Index>>& faces) const {
    positions.clear();
    faces.clear();
    std::vector<Index> remap(m_vertices.size(), kInvalidIndex);
    for (Index i = 0; i < m_vertices.size(); ++i) {
        if (m_vertices[i].alive) {
            remap[i] = static_cast<Index>(positions.size());
            positions.push_back(m_vertices[i].position);
        }
    }
    for (Index i = 0; i < m_faces.size(); ++i) {
        if (!m_faces[i].alive) {
            continue;
        }
        std::vector<Index> face;
        face.reserve(m_faces[i].size);
        for (const VertexId v : faceVertices({i})) {
            face.push_back(remap[v.value]);
        }
        faces.push_back(std::move(face));
    }
}

// ---- adjacency queries -----------------------------------------------------

std::vector<LoopId> Mesh::faceLoops(FaceId face) const {
    std::vector<LoopId> loops;
    loops.reserve(m_faces[face.value].size);
    const LoopId first = m_faces[face.value].first;
    LoopId l = first;
    do {
        loops.push_back(l);
        l = m_loops[l.value].next;
    } while (!(l == first));
    return loops;
}

std::vector<VertexId> Mesh::faceVertices(FaceId face) const {
    std::vector<VertexId> vertices;
    vertices.reserve(m_faces[face.value].size);
    for (const LoopId l : faceLoops(face)) {
        vertices.push_back(m_loops[l.value].vertex);
    }
    return vertices;
}

EdgeId Mesh::edgeBetween(VertexId a, VertexId b) const {
    if (!isAlive(a) || !isAlive(b)) {
        return {};
    }
    for (const EdgeId e : m_vertices[a.value].edges) {
        const Edge& edge = m_edges[e.value];
        if ((edge.v0 == a && edge.v1 == b) || (edge.v0 == b && edge.v1 == a)) {
            return e;
        }
    }
    return {};
}

std::vector<FaceId> Mesh::edgeFaces(EdgeId e) const {
    std::vector<FaceId> faces;
    const LoopId head = m_edges[e.value].radial;
    if (!head.valid()) {
        return faces;
    }
    LoopId l = head;
    do {
        faces.push_back(m_loops[l.value].face);
        l = m_loops[l.value].radialNext;
    } while (!(l == head));
    return faces;
}

std::size_t Mesh::edgeFaceCount(EdgeId e) const {
    std::size_t count = 0;
    const LoopId head = m_edges[e.value].radial;
    if (!head.valid()) {
        return 0;
    }
    LoopId l = head;
    do {
        ++count;
        l = m_loops[l.value].radialNext;
    } while (!(l == head));
    return count;
}

std::vector<FaceId> Mesh::vertexFaces(VertexId v) const {
    std::vector<FaceId> faces;
    for (const EdgeId e : m_vertices[v.value].edges) {
        for (const FaceId f : edgeFaces(e)) {
            if (std::find_if(faces.begin(), faces.end(), [f](FaceId x) { return x == f; }) ==
                faces.end()) {
                faces.push_back(f);
            }
        }
    }
    std::sort(faces.begin(), faces.end(), [](FaceId a, FaceId b) { return a.value < b.value; });
    return faces;
}

Vec3 Mesh::faceNormal(FaceId face) const {
    // Newell's method: robust for non-planar n-gons.
    Vec3 normal{};
    const std::vector<VertexId> verts = faceVertices(face);
    for (std::size_t i = 0; i < verts.size(); ++i) {
        const Vec3 a = position(verts[i]);
        const Vec3 b = position(verts[(i + 1) % verts.size()]);
        normal.x += (a.y - b.y) * (a.z + b.z);
        normal.y += (a.z - b.z) * (a.x + b.x);
        normal.z += (a.x - b.x) * (a.y + b.y);
    }
    return normalized(normal);
}

Vec3 Mesh::faceCentroid(FaceId face) const {
    Vec3 sum{};
    const std::vector<VertexId> verts = faceVertices(face);
    for (const VertexId v : verts) {
        sum += position(v);
    }
    return sum / static_cast<float>(verts.size());
}

}  // namespace cyber
