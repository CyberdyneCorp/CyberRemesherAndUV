#include <algorithm>
#include <string>

#include "cyber/core/mesh.hpp"

namespace cyber {

std::vector<std::string> Mesh::validate() const {
    std::vector<std::string> errors;
    auto fail = [&errors](const std::string& what, Index i) {
        errors.push_back(what + " #" + std::to_string(i));
    };

    std::size_t aliveV = 0, aliveE = 0, aliveF = 0, aliveL = 0;

    for (Index i = 0; i < m_loops.size(); ++i) {
        const Loop& l = m_loops[i];
        if (!l.alive) {
            continue;
        }
        ++aliveL;
        if (!isAlive(l.vertex)) {
            fail("loop with dead vertex", i);
            continue;
        }
        if (!isAlive(l.face)) {
            fail("loop with dead face", i);
            continue;
        }
        if (!l.edge.valid() || !m_edges[l.edge.value].alive) {
            fail("loop with dead edge", i);
            continue;
        }
        if (!(m_loops[l.next.value].prev == LoopId{i})) {
            fail("broken next/prev reciprocity at loop", i);
        }
        if (!(m_loops[l.radialNext.value].radialPrev == LoopId{i})) {
            fail("broken radial reciprocity at loop", i);
        }
        if (!(m_loops[l.radialNext.value].edge == l.edge)) {
            fail("radial neighbor on different edge at loop", i);
        }
        if (!(m_loops[l.next.value].face == l.face)) {
            fail("face cycle crosses faces at loop", i);
        }
        // loop.edge must connect this corner to the next corner.
        const Edge& e = m_edges[l.edge.value];
        const VertexId u = l.vertex;
        const VertexId w = m_loops[l.next.value].vertex;
        const bool matches = (e.v0 == u && e.v1 == w) || (e.v0 == w && e.v1 == u);
        if (!matches) {
            fail("loop edge does not connect its corners at loop", i);
        }
    }

    for (Index i = 0; i < m_faces.size(); ++i) {
        const Face& f = m_faces[i];
        if (!f.alive) {
            continue;
        }
        ++aliveF;
        if (!f.first.valid() || !m_loops[f.first.value].alive) {
            fail("face with dead first loop", i);
            continue;
        }
        Index steps = 0;
        LoopId l = f.first;
        do {
            ++steps;
            l = m_loops[l.value].next;
        } while (!(l == f.first) && steps <= f.size + 1);
        if (steps != f.size) {
            fail("face size does not match its loop cycle at face", i);
        }
        if (f.size < 3) {
            fail("face with fewer than 3 corners", i);
        }
    }

    for (Index i = 0; i < m_edges.size(); ++i) {
        const Edge& e = m_edges[i];
        if (!e.alive) {
            continue;
        }
        ++aliveE;
        if (e.v0 == e.v1) {
            fail("edge with identical endpoints", i);
        }
        if (!isAlive(e.v0) || !isAlive(e.v1)) {
            fail("edge with dead endpoint", i);
            continue;
        }
        for (const VertexId v : {e.v0, e.v1}) {
            const auto& list = m_vertices[v.value].edges;
            std::size_t count = 0;
            for (const EdgeId le : list) {
                if (le == EdgeId{i}) {
                    ++count;
                }
            }
            if (count != 1) {
                fail("edge not registered exactly once on its vertex, edge", i);
            }
        }
        if (!e.radial.valid()) {
            fail("edge with no incident face (wire edges unsupported)", i);
        }
    }

    for (Index i = 0; i < m_vertices.size(); ++i) {
        const Vertex& v = m_vertices[i];
        if (!v.alive) {
            continue;
        }
        ++aliveV;
        for (const EdgeId e : v.edges) {
            if (!e.valid() || !m_edges[e.value].alive) {
                fail("vertex listing dead edge, vertex", i);
                continue;
            }
            const Edge& edge = m_edges[e.value];
            if (!(edge.v0 == VertexId{i}) && !(edge.v1 == VertexId{i})) {
                fail("vertex listing foreign edge, vertex", i);
            }
        }
    }

    if (aliveV != m_aliveVertices) {
        fail("alive vertex counter mismatch, expected", static_cast<Index>(aliveV));
    }
    if (aliveE != m_aliveEdges) {
        fail("alive edge counter mismatch, expected", static_cast<Index>(aliveE));
    }
    if (aliveF != m_aliveFaces) {
        fail("alive face counter mismatch, expected", static_cast<Index>(aliveF));
    }
    if (aliveL != m_aliveLoops) {
        fail("alive loop counter mismatch, expected", static_cast<Index>(aliveL));
    }
    return errors;
}

std::vector<std::vector<FaceId>> Mesh::islands() const {
    std::vector<std::vector<FaceId>> result;
    std::vector<bool> visited(m_faces.size(), false);
    std::vector<FaceId> stack;

    for (Index seed = 0; seed < m_faces.size(); ++seed) {
        if (!m_faces[seed].alive || visited[seed]) {
            continue;
        }
        std::vector<FaceId> island;
        stack.push_back({seed});
        visited[seed] = true;
        while (!stack.empty()) {
            const FaceId f = stack.back();
            stack.pop_back();
            island.push_back(f);
            for (const LoopId l : faceLoops(f)) {
                // Every face sharing this loop's edge, via the radial cycle:
                // non-manifold edges connect all their faces (face-complete
                // island detection, mesh-core spec).
                const LoopId head = l;
                LoopId r = m_loops[l.value].radialNext;
                while (!(r == head)) {
                    const FaceId neighbor = m_loops[r.value].face;
                    if (!visited[neighbor.value]) {
                        visited[neighbor.value] = true;
                        stack.push_back(neighbor);
                    }
                    r = m_loops[r.value].radialNext;
                }
            }
        }
        std::sort(island.begin(), island.end(),
                  [](FaceId a, FaceId b) { return a.value < b.value; });
        result.push_back(std::move(island));
    }
    return result;
}

void Mesh::tagFeatureEdges(float dihedralAngleDegrees) {
    // An edge is a feature when its included (dihedral) angle is at most the
    // threshold: equivalently the angle between face normals is at least
    // 180 - threshold. Boundary and non-manifold edges are always features.
    const float normalAngleThreshold = degreesToRadians(180.0f - dihedralAngleDegrees);
    for (Index i = 0; i < m_edges.size(); ++i) {
        if (!m_edges[i].alive) {
            continue;
        }
        const EdgeId e{i};
        const std::vector<FaceId> faces = edgeFaces(e);
        if (faces.size() != 2) {
            m_edges[i].feature = true;
            continue;
        }
        const Vec3 n0 = faceNormal(faces[0]);
        const Vec3 n1 = faceNormal(faces[1]);
        const float cosAngle = std::clamp(dot(n0, n1), -1.0f, 1.0f);
        const float normalAngle = std::acos(cosAngle);
        m_edges[i].feature = normalAngle >= normalAngleThreshold;
    }
}

}  // namespace cyber
