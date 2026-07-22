#include <algorithm>
#include <cassert>
#include <unordered_map>
#include <unordered_set>

#include "cyber/core/mesh.hpp"

namespace cyber {

namespace {

// Cyclically removes consecutive duplicates from a corner list.
void dedupeCyclic(std::vector<VertexId>& verts) {
    bool changed = true;
    while (changed && verts.size() > 1) {
        changed = false;
        for (std::size_t i = 0; i < verts.size(); ++i) {
            if (verts[i] == verts[(i + 1) % verts.size()]) {
                verts.erase(verts.begin() + static_cast<std::ptrdiff_t>(i));
                changed = true;
                break;
            }
        }
    }
}

bool hasDuplicates(const std::vector<VertexId>& verts) {
    for (std::size_t i = 0; i < verts.size(); ++i) {
        for (std::size_t j = i + 1; j < verts.size(); ++j) {
            if (verts[i] == verts[j]) {
                return true;
            }
        }
    }
    return false;
}

}  // namespace

bool Mesh::mergeVertices(VertexId keep, VertexId remove) {
    if (!isAlive(keep) || !isAlive(remove) || keep == remove) {
        return false;
    }

    // Capture edge metadata around `remove` keyed by the opposite vertex so
    // feature flags / edge attributes survive the rebuild.
    struct EdgeMemo {
        VertexId other;
        bool feature;
        AttributeSet::Row attrs;
    };
    std::vector<EdgeMemo> edgeMemos;
    for (const EdgeId e : m_vertices[remove.value].edges) {
        const Edge& edge = m_edges[e.value];
        const VertexId other = edge.v0 == remove ? edge.v1 : edge.v0;
        edgeMemos.push_back({other, edge.feature, m_edgeAttrs.extractRow(e.value)});
    }

    // Faces incident to `remove`, rebuilt with the merged corner. Corner
    // attribute rows are captured per (face, position).
    struct FaceMemo {
        std::vector<VertexId> verts;
        std::vector<AttributeSet::Row> corners;
        AttributeSet::Row attrs;
    };
    std::vector<FaceId> affected = vertexFaces(remove);
    std::vector<FaceMemo> memos;
    memos.reserve(affected.size());
    for (const FaceId f : affected) {
        FaceMemo memo;
        for (const LoopId l : faceLoops(f)) {
            VertexId v = m_loops[l.value].vertex;
            memo.verts.push_back(v == remove ? keep : v);
            memo.corners.push_back(m_cornerAttrs.extractRow(l.value));
        }
        memo.attrs = m_faceAttrs.extractRow(f.value);
        memos.push_back(std::move(memo));
    }
    for (const FaceId f : affected) {
        removeFace(f);
    }

    for (FaceMemo& memo : memos) {
        // Drop corners that became consecutive duplicates (keeping their
        // paired attribute rows aligned).
        std::vector<VertexId> verts = memo.verts;
        dedupeCyclic(verts);
        if (verts.size() < 3 || hasDuplicates(verts)) {
            // Degenerate or self-touching after the merge: face is deleted
            // (documented policy).
            continue;
        }
        const FaceId nf = addFace(verts);
        if (!nf.valid()) {
            continue;
        }
        m_faceAttrs.applyRow(nf.value, memo.attrs);
        const std::vector<LoopId> loops = faceLoops(nf);
        // Corner rows re-applied by matching vertex within the original
        // corner order.
        for (const LoopId l : loops) {
            const VertexId v = m_loops[l.value].vertex;
            for (std::size_t i = 0; i < memo.verts.size(); ++i) {
                if (memo.verts[i] == v) {
                    m_cornerAttrs.applyRow(l.value, memo.corners[i]);
                    break;
                }
            }
        }
    }

    // Restore edge metadata onto merged edges where they still exist.
    for (const EdgeMemo& memo : edgeMemos) {
        if (memo.other == keep) {
            continue;
        }
        const EdgeId e = edgeBetween(keep, memo.other);
        if (e.valid()) {
            m_edges[e.value].feature = m_edges[e.value].feature || memo.feature;
            m_edgeAttrs.applyRow(e.value, memo.attrs);
        }
    }

    if (isAlive(remove) && m_vertices[remove.value].edges.empty()) {
        freeVertex(remove);
    }
    return true;
}

bool Mesh::collapseEdge(EdgeId edge, bool placeAtMidpoint) {
    if (!isAlive(edge)) {
        return false;
    }
    const auto [a, b] = edgeVertices(edge);
    const Vec3 midpoint = lerp(position(a), position(b), 0.5f);
    if (!mergeVertices(a, b)) {
        return false;
    }
    if (placeAtMidpoint && isAlive(a)) {
        setPosition(a, midpoint);
        m_vertexAttrs.interpolate(a.value, a.value, a.value, 0.0f);
    }
    return true;
}

bool Mesh::flipEdge(EdgeId edge) {
    if (!isAlive(edge) || edgeFaceCount(edge) != 2) {
        return false;
    }
    const std::vector<FaceId> faces = edgeFaces(edge);
    const FaceId f0 = faces[0], f1 = faces[1];
    if (faceSize(f0) != 3 || faceSize(f1) != 3) {
        return false;
    }
    const auto [a, b] = edgeVertices(edge);

    auto oppositeVertex = [this, va = a, vb = b](FaceId f) -> VertexId {
        for (const VertexId v : faceVertices(f)) {
            if (!(v == va) && !(v == vb)) {
                return v;
            }
        }
        return {};
    };
    const VertexId c = oppositeVertex(f0);
    const VertexId d = oppositeVertex(f1);
    if (!c.valid() || !d.valid() || c == d) {
        return false;
    }
    if (edgeBetween(c, d).valid()) {
        // Flip would create a duplicate edge (fold-over).
        return false;
    }

    // Determine winding: find in f0 whether the loop at `a` points to `b`.
    bool aToB = false;
    for (const LoopId l : faceLoops(f0)) {
        if (m_loops[l.value].edge == edge) {
            aToB = m_loops[l.value].vertex == a;
            break;
        }
    }
    const VertexId u = aToB ? a : b;  // f0 = (u, w, c) with u->w the edge
    const VertexId w = aToB ? b : a;

    const AttributeSet::Row attrs0 = m_faceAttrs.extractRow(f0.value);
    const AttributeSet::Row attrs1 = m_faceAttrs.extractRow(f1.value);
    struct FeatureMemo {
        VertexId x, y;
        bool feature;
    };
    std::vector<FeatureMemo> features;
    for (const auto& [x, y] : {std::pair{u, c}, {c, w}, {w, d}, {d, u}}) {
        const EdgeId e = edgeBetween(x, y);
        if (e.valid()) {
            features.push_back({x, y, m_edges[e.value].feature});
        }
    }

    removeFace(f0);
    removeFace(f1);
    // Quad cycle u -> d -> w -> c, new diagonal c-d.
    const FaceId n0 = addFace(std::array{u, d, c});
    const FaceId n1 = addFace(std::array{d, w, c});
    assert(n0.valid() && n1.valid());
    m_faceAttrs.applyRow(n0.value, attrs0);
    m_faceAttrs.applyRow(n1.value, attrs1);
    for (const FeatureMemo& memo : features) {
        const EdgeId e = edgeBetween(memo.x, memo.y);
        if (e.valid()) {
            m_edges[e.value].feature = memo.feature;
        }
    }
    return true;
}

VertexId Mesh::splitEdge(EdgeId edge, float t) {
    if (!isAlive(edge)) {
        return {};
    }
    const auto [a, b] = edgeVertices(edge);
    const bool feature = m_edges[edge.value].feature;
    const AttributeSet::Row edgeAttrs = m_edgeAttrs.extractRow(edge.value);

    const VertexId m = addVertex(lerp(position(a), position(b), t));
    m_vertexAttrs.interpolate(m.value, a.value, b.value, t);

    // Snapshot the radial loops before we start relinking.
    std::vector<LoopId> radials;
    {
        const LoopId head = m_edges[edge.value].radial;
        if (head.valid()) {
            LoopId l = head;
            do {
                radials.push_back(l);
                l = m_loops[l.value].radialNext;
            } while (!(l == head));
        }
    }

    const EdgeId ea = findOrCreateEdge(a, m);
    const EdgeId eb = findOrCreateEdge(m, b);
    for (const EdgeId half : {ea, eb}) {
        m_edges[half.value].feature = feature;
        m_edgeAttrs.applyRow(half.value, edgeAttrs);
    }

    for (const LoopId l : radials) {
        Loop& loop = m_loops[l.value];
        const FaceId f = loop.face;
        const LoopId nextLoop = loop.next;
        const bool forward = loop.vertex == a;  // loop runs a -> b

        radialRemove(l);
        const LoopId lm = allocLoop();
        Loop& mid = m_loops[lm.value];
        mid.vertex = m;
        mid.face = f;
        mid.next = nextLoop;
        mid.prev = l;
        m_loops[nextLoop.value].prev = lm;
        m_loops[l.value].next = lm;

        radialInsert(forward ? ea : eb, l);
        radialInsert(forward ? eb : ea, lm);

        m_cornerAttrs.interpolate(lm.value, l.value, nextLoop.value, forward ? t : 1.0f - t);
        ++m_faces[f.value].size;
    }

    destroyEdgeIfUnused(edge);
    return m;
}

FaceId Mesh::splitFace(FaceId face, VertexId a, VertexId b) {
    if (!isAlive(face) || a == b) {
        return {};
    }
    LoopId la{}, lb{};
    for (const LoopId l : faceLoops(face)) {
        if (m_loops[l.value].vertex == a) {
            la = l;
        } else if (m_loops[l.value].vertex == b) {
            lb = l;
        }
    }
    if (!la.valid() || !lb.valid()) {
        return {};
    }
    if (m_loops[la.value].next == lb || m_loops[lb.value].next == la) {
        return {};  // adjacent corners: the diagonal already exists as an edge
    }

    const EdgeId diagonal = findOrCreateEdge(a, b);

    // Segment S1: la .. lb.prev stays in `face`; segment S2: lb .. la.prev
    // moves to the new face.
    const LoopId laPrev = m_loops[la.value].prev;
    const LoopId lbPrev = m_loops[lb.value].prev;

    const LoopId closeF = allocLoop();  // corner at b closing `face` back to a
    const LoopId closeN = allocLoop();  // corner at a closing the new face back to b
    const FaceId newFace = allocFace();

    // Close original face: la .. lbPrev -> closeF -> la
    m_loops[closeF.value].vertex = b;
    m_loops[closeF.value].face = face;
    m_loops[closeF.value].next = la;
    m_loops[closeF.value].prev = lbPrev;
    m_loops[lbPrev.value].next = closeF;
    m_loops[la.value].prev = closeF;
    radialInsert(diagonal, closeF);

    // Close new face: lb .. laPrev -> closeN -> lb
    m_loops[closeN.value].vertex = a;
    m_loops[closeN.value].face = newFace;
    m_loops[closeN.value].next = lb;
    m_loops[closeN.value].prev = laPrev;
    m_loops[laPrev.value].next = closeN;
    m_loops[lb.value].prev = closeN;
    radialInsert(diagonal, closeN);

    // Reassign faces and recount sizes.
    Index size1 = 0;
    {
        LoopId l = la;
        do {
            m_loops[l.value].face = face;
            ++size1;
            l = m_loops[l.value].next;
        } while (!(l == la));
    }
    Index size2 = 0;
    {
        LoopId l = lb;
        do {
            m_loops[l.value].face = newFace;
            ++size2;
            l = m_loops[l.value].next;
        } while (!(l == lb));
    }
    m_faces[face.value].first = la;
    m_faces[face.value].size = size1;
    m_faces[newFace.value].first = lb;
    m_faces[newFace.value].size = size2;

    m_faceAttrs.copy(newFace.value, face.value);
    m_cornerAttrs.copy(closeF.value, lb.value);
    m_cornerAttrs.copy(closeN.value, la.value);
    return newFace;
}

void Mesh::triangulateFace(FaceId face) {
    FaceId current = face;
    while (isAlive(current) && faceSize(current) > 3) {
        const std::vector<VertexId> verts = faceVertices(current);
        // Split off the ear (v0, v1, v2); continue with the remainder.
        const FaceId rest = splitFace(current, verts[0], verts[2]);
        if (!rest.valid()) {
            return;
        }
        current = rest;
    }
}

void Mesh::triangulate() {
    const std::size_t n = m_faces.size();
    for (Index i = 0; i < n; ++i) {
        if (m_faces[i].alive) {
            triangulateFace({i});
        }
    }
}

std::size_t Mesh::fillHoles(std::size_t maxBoundaryEdges) {
    if (maxBoundaryEdges < 3) {
        return 0;
    }
    // Directed hole edges: for each boundary loop L the incident face traverses
    // its edge a -> b, so the hole (open) side runs b -> a. succ[b] = a chains
    // those directed edges around the hole; a vertex that would get two
    // successors marks a non-manifold (branching) boundary we refuse to fill.
    std::unordered_map<Index, Index> succ;
    std::unordered_set<Index> ambiguous;
    for (Index fi = 0; fi < m_faces.size(); ++fi) {
        if (!m_faces[fi].alive) {
            continue;
        }
        for (const LoopId l : faceLoops(FaceId{fi})) {
            if (edgeFaceCount(m_loops[l.value].edge) != 1) {
                continue;
            }
            const Index a = m_loops[l.value].vertex.value;
            const Index b = m_loops[m_loops[l.value].next.value].vertex.value;
            if (!succ.emplace(b, a).second) {
                ambiguous.insert(b);
            }
        }
    }

    std::size_t filled = 0;
    std::unordered_set<Index> visited;
    for (const auto& [start, unused] : succ) {
        (void)unused;
        if (visited.count(start)) {
            continue;
        }
        std::vector<VertexId> loop;
        Index cur = start;
        bool ok = true;
        while (true) {
            if (ambiguous.count(cur)) {
                ok = false;
                break;
            }
            visited.insert(cur);
            loop.push_back(VertexId{cur});
            const auto it = succ.find(cur);
            if (it == succ.end()) {
                ok = false;  // open chain, not a closed hole
                break;
            }
            cur = it->second;
            if (cur == start) {
                break;  // closed the loop
            }
            if (loop.size() > maxBoundaryEdges) {
                ok = false;  // hole larger than the limit
                break;
            }
        }
        if (ok && loop.size() >= 3 && loop.size() <= maxBoundaryEdges && addFace(loop).valid()) {
            ++filled;
        }
    }
    return filled;
}

Mesh Mesh::linearSubdivide() const {
    Mesh out;
    out.vertexAttributes().adoptSchema(m_vertexAttrs);
    out.edgeAttributes().adoptSchema(m_edgeAttrs);
    out.faceAttributes().adoptSchema(m_faceAttrs);
    out.cornerAttributes().adoptSchema(m_cornerAttrs);

    // Original vertices.
    std::vector<VertexId> vertexMap(m_vertices.size());
    for (Index i = 0; i < m_vertices.size(); ++i) {
        if (!m_vertices[i].alive) {
            continue;
        }
        vertexMap[i] = out.addVertex(m_vertices[i].position);
        out.vertexAttributes().applyRow(vertexMap[i].value, m_vertexAttrs.extractRow(i));
    }
    // Edge midpoints.
    std::vector<VertexId> midMap(m_edges.size());
    for (Index i = 0; i < m_edges.size(); ++i) {
        if (!m_edges[i].alive) {
            continue;
        }
        const Edge& e = m_edges[i];
        midMap[i] = out.addVertex(lerp(position(e.v0), position(e.v1), 0.5f));
        out.vertexAttributes().applyRow(
            midMap[i].value, AttributeSet::averageRows({m_vertexAttrs.extractRow(e.v0.value),
                                                        m_vertexAttrs.extractRow(e.v1.value)}));
    }

    for (Index fi = 0; fi < m_faces.size(); ++fi) {
        if (!m_faces[fi].alive) {
            continue;
        }
        const FaceId f{fi};
        const std::vector<LoopId> loops = faceLoops(f);
        const std::size_t n = loops.size();

        const VertexId center = out.addVertex(faceCentroid(f));
        {
            std::vector<AttributeSet::Row> rows;
            rows.reserve(n);
            for (const LoopId l : loops) {
                rows.push_back(m_vertexAttrs.extractRow(m_loops[l.value].vertex.value));
            }
            out.vertexAttributes().applyRow(center.value, AttributeSet::averageRows(rows));
        }
        std::vector<AttributeSet::Row> cornerRows;
        cornerRows.reserve(n);
        for (const LoopId l : loops) {
            cornerRows.push_back(m_cornerAttrs.extractRow(l.value));
        }
        AttributeSet::Row centerCorner = AttributeSet::averageRows(cornerRows);
        const AttributeSet::Row faceRow = m_faceAttrs.extractRow(fi);

        for (std::size_t i = 0; i < n; ++i) {
            const std::size_t prev = (i + n - 1) % n;
            const LoopId li = loops[i];
            const VertexId vi = m_loops[li.value].vertex;
            const EdgeId eNext = m_loops[li.value].edge;
            const EdgeId ePrev = m_loops[loops[prev].value].edge;

            const FaceId child = out.addFace(
                std::array{vertexMap[vi.value], midMap[eNext.value], center, midMap[ePrev.value]});
            if (!child.valid()) {
                continue;
            }
            out.faceAttributes().applyRow(child.value, faceRow);
            // Child corners: original corner at vi; averaged corners at the
            // midpoints and center.
            const std::vector<LoopId> childLoops = out.faceLoops(child);
            out.cornerAttributes().applyRow(childLoops[0].value, cornerRows[i]);
            out.cornerAttributes().applyRow(
                childLoops[1].value,
                AttributeSet::averageRows({cornerRows[i], cornerRows[(i + 1) % n]}));
            out.cornerAttributes().applyRow(childLoops[2].value, centerCorner);
            out.cornerAttributes().applyRow(
                childLoops[3].value, AttributeSet::averageRows({cornerRows[prev], cornerRows[i]}));

            // Feature flags: child boundary edges inherit their parent edge.
            for (const auto& [parentEdge, x, y] :
                 {std::tuple{eNext, vertexMap[vi.value], midMap[eNext.value]},
                  std::tuple{ePrev, midMap[ePrev.value], vertexMap[vi.value]}}) {
                if (m_edges[parentEdge.value].feature) {
                    const EdgeId ce = out.edgeBetween(x, y);
                    if (ce.valid()) {
                        out.setFeatureEdge(ce, true);
                    }
                }
            }
        }
    }
    return out;
}

}  // namespace cyber
