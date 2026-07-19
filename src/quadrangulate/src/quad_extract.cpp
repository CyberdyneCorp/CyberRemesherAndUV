// Instant-Meshes-style quad extraction from the orientation + position fields.
// Clean-room reimplementation of the mesh-extraction algorithm in
// "Instant Field-Aligned Meshes" (Jakob, Tarini, Panozzo, Sorkine-Hornung,
// SIGGRAPH Asia 2015) / wjakob/instant-meshes (BSD-3-Clause). No source was
// copied; see THIRD_PARTY_NOTICES.md.
//
// Pipeline: (A) collapse mesh vertices into lattice cells and build a graph of
// distance-1 lattice edges; (B) trace faces by walking the angularly-ordered
// graph, preferring quads; (C) clean up. Extraction is entirely local — no
// mixed-integer / holonomy solve — so field singularities surface as irregular
// vertices automatically.

#include "cyber/quadrangulate/position_field.hpp"

#include <algorithm>
#include <array>
#include <cmath>
#include <cstdint>
#include <numeric>
#include <vector>

namespace cyber::remesh {

namespace {

struct Int2 {
    int x = 0;
    int y = 0;
};

// The two crosses rotated into a common representative: returns q0's chosen
// representative and q1's matching (sign-aligned) representative.
std::pair<Vec3, Vec3> compatOrientation(Vec3 q0, Vec3 n0, Vec3 q1, Vec3 n1) {
    const std::array<Vec3, 2> a{q0, cross(n0, q0)};
    const std::array<Vec3, 2> b{q1, cross(n1, q1)};
    float best = -1.0f;
    int ba = 0, bb = 0;
    for (int i = 0; i < 2; ++i) {
        for (int j = 0; j < 2; ++j) {
            const float d = std::fabs(dot(a[static_cast<std::size_t>(i)], b[static_cast<std::size_t>(j)]));
            if (d > best) {
                best = d;
                ba = i;
                bb = j;
            }
        }
    }
    const Vec3 ra = a[static_cast<std::size_t>(ba)];
    const Vec3 rb = b[static_cast<std::size_t>(bb)];
    return {ra, dot(ra, rb) < 0.0f ? rb * -1.0f : rb};
}

// Least-squares point lying in both tangent planes (through p0 with normal n0,
// and p1 with normal n1). Falls back to the midpoint when the planes are near
// parallel.
Vec3 middlePoint(Vec3 p0, Vec3 n0, Vec3 p1, Vec3 n1) {
    const float n0p0 = dot(n0, p0), n0p1 = dot(n0, p1);
    const float n1p0 = dot(n1, p0), n1p1 = dot(n1, p1);
    const float n0n1 = dot(n0, n1);
    const float denom = 1.0f - n0n1 * n0n1;
    if (std::fabs(denom) < 1e-4f) {
        return (p0 + p1) * 0.5f;
    }
    const float l0 = 0.5f * (n0p1 - n0p0 - n0n1 * (n1p0 - n1p1)) / denom;
    const float l1 = 0.5f * (n1p0 - n1p1 - n0n1 * (n0p1 - n0p0)) / denom;
    return (p0 + p1) * 0.5f + (n0 * l0 + n1 * l1) * 0.5f;
}

// Integer lattice cell (floor) of `target` in the frame (o; q, t=n x q) at
// spacing s.
Int2 floorIndex(Vec3 o, Vec3 q, Vec3 t, Vec3 target, float invS) {
    const Vec3 d = target - o;
    return {static_cast<int>(std::floor(dot(q, d) * invS)),
            static_cast<int>(std::floor(dot(t, d) * invS))};
}

struct PosCompat {
    Int2 i0;
    Int2 i1;
    float err = 0.0f;
};

// The pair of integer lattice indices (one per vertex) whose lattice points are
// closest, and that closest squared distance. Searches the 4 corners of each
// vertex's floor cell around the shared middle point.
PosCompat compatPosition(Vec3 p0, Vec3 n0, Vec3 q0, Vec3 o0, Vec3 p1, Vec3 n1, Vec3 q1, Vec3 o1,
                         float s, float invS) {
    const Vec3 t0 = cross(n0, q0);
    const Vec3 t1 = cross(n1, q1);
    const Vec3 mid = middlePoint(p0, n0, p1, n1);
    const Int2 f0 = floorIndex(o0, q0, t0, mid, invS);
    const Int2 f1 = floorIndex(o1, q1, t1, mid, invS);
    float best = 1e30f;
    int bi = 0, bj = 0;
    for (int i = 0; i < 4; ++i) {
        const Vec3 o0t =
            o0 + (q0 * static_cast<float>((i & 1) + f0.x) + t0 * static_cast<float>(((i & 2) >> 1) + f0.y)) * s;
        for (int j = 0; j < 4; ++j) {
            const Vec3 o1t =
                o1 + (q1 * static_cast<float>((j & 1) + f1.x) + t1 * static_cast<float>(((j & 2) >> 1) + f1.y)) * s;
            const float d = lengthSquared(o0t - o1t);
            if (d < best) {
                best = d;
                bi = i;
                bj = j;
            }
        }
    }
    return {{f0.x + (bi & 1), f0.y + ((bi & 2) >> 1)}, {f1.x + (bj & 1), f1.y + ((bj & 2) >> 1)}, best};
}

// Union-find with union-by-size; unite() returns the surviving representative.
struct DisjointSets {
    std::vector<Index> parent;
    std::vector<Index> size;
    explicit DisjointSets(std::size_t n) : parent(n), size(n, 1) {
        std::iota(parent.begin(), parent.end(), Index{0});
    }
    Index find(Index x) {
        while (parent[x] != x) {
            parent[x] = parent[parent[x]];
            x = parent[x];
        }
        return x;
    }
    Index unite(Index a, Index b) {
        a = find(a);
        b = find(b);
        if (a == b) {
            return a;
        }
        if (size[a] < size[b]) {
            std::swap(a, b);
        }
        parent[b] = a;
        size[a] += size[b];
        return a;
    }
};

// An orthonormal tangent basis (s, t) of the plane normal to n.
void coordinateSystem(Vec3 n, Vec3& s, Vec3& t) {
    if (std::fabs(n.x) > std::fabs(n.y)) {
        const float il = 1.0f / std::sqrt(n.x * n.x + n.z * n.z);
        s = Vec3{-n.z * il, 0.0f, n.x * il};
    } else {
        const float il = 1.0f / std::sqrt(n.y * n.y + n.z * n.z);
        s = Vec3{0.0f, n.z * il, -n.y * il};
    }
    t = cross(n, s);
}

// A directed graph link with a "used" flag for the face walk.
struct Link {
    Index id = kInvalidIndex;
    std::uint8_t flag = 0;
};

// The collapsed lattice graph: one node per lattice cell, plus per-node
// angularly-ordered adjacency of distance-1 lattice edges.
struct Graph {
    std::vector<Vec3> pos;
    std::vector<Vec3> normal;
    std::vector<std::vector<Link>> adj;
};

Graph buildCollapsedGraph(const Mesh& mesh, const PositionField& field) {
    const float s = field.spacing;
    const float invS = 1.0f / s;
    const std::size_t nV = field.size();

    // --- A1: classify every mesh edge as collapse candidate / lattice edge. ---
    struct CollapseEdge {
        Index i;
        Index j;
        float err;
    };
    std::vector<CollapseEdge> collapse;
    std::vector<std::vector<Index>> latticeAdj(nV);
    for (Index ei = 0; ei < mesh.edgeCapacity(); ++ei) {
        const EdgeId e{ei};
        if (!mesh.isAlive(e)) {
            continue;
        }
        const auto [va, vb] = mesh.edgeVertices(e);
        const std::size_t i = va.value;
        const std::size_t j = vb.value;
        if (!field.valid[i] || !field.valid[j]) {
            continue;
        }
        const auto [q0, q1] =
            compatOrientation(field.q[i], field.normal[i], field.q[j], field.normal[j]);
        const PosCompat pc =
            compatPosition(mesh.position(va), field.normal[i], q0, field.o[i], mesh.position(vb),
                           field.normal[j], q1, field.o[j], s, invS);
        const int dx = std::abs(pc.i0.x - pc.i1.x);
        const int dy = std::abs(pc.i0.y - pc.i1.y);
        if (std::max(dx, dy) > 1 || (dx == 1 && dy == 1)) {
            continue;  // too far apart, or a quad diagonal
        }
        if (dx + dy == 0) {
            collapse.push_back({va.value, vb.value, pc.err});
        } else {
            latticeAdj[i].push_back(static_cast<Index>(j));
            latticeAdj[j].push_back(static_cast<Index>(i));
        }
    }

    // --- A2: error-ordered collapse with the anti-chaining conflict check. ---
    std::sort(collapse.begin(), collapse.end(), [](const CollapseEdge& a, const CollapseEdge& b) {
        return a.err != b.err ? a.err < b.err : (a.i != b.i ? a.i < b.i : a.j < b.j);
    });
    DisjointSets dset(nV);
    std::vector<std::vector<Index>> adjNew = latticeAdj;  // indexed by representative
    for (const CollapseEdge& ce : collapse) {
        const Index ri = dset.find(ce.i);
        const Index rj = dset.find(ce.j);
        if (ri == rj) {
            continue;
        }
        bool conflict = false;
        for (const Index n : adjNew[ri]) {
            if (dset.find(n) == rj) {
                conflict = true;  // already a lattice step apart: merging would fuse a whole row
                break;
            }
        }
        if (conflict) {
            continue;
        }
        const Index r = dset.unite(ri, rj);
        const Index other = (r == ri) ? rj : ri;
        std::vector<Index> merged;
        for (const std::vector<Index>* src : {&adjNew[ri], &adjNew[rj]}) {
            for (const Index n : *src) {
                const Index rn = dset.find(n);
                if (rn != r) {
                    merged.push_back(rn);
                }
            }
        }
        std::sort(merged.begin(), merged.end());
        merged.erase(std::unique(merged.begin(), merged.end()), merged.end());
        adjNew[r] = std::move(merged);
        adjNew[other].clear();
    }

    // --- A3: compact clusters into node ids. ---
    std::vector<Index> nodeOf(nV, kInvalidIndex);
    Graph g;
    std::vector<float> weight;
    for (std::size_t v = 0; v < nV; ++v) {
        if (!field.valid[v]) {
            continue;
        }
        const Index r = dset.find(static_cast<Index>(v));
        if (nodeOf[r] == kInvalidIndex) {
            nodeOf[r] = static_cast<Index>(g.pos.size());
            g.pos.push_back(Vec3{0, 0, 0});
            g.normal.push_back(Vec3{0, 0, 0});
            weight.push_back(0.0f);
        }
    }

    // --- A4: node positions/normals = member-weighted centroid. ---
    for (std::size_t v = 0; v < nV; ++v) {
        if (!field.valid[v]) {
            continue;
        }
        const Index node = nodeOf[dset.find(static_cast<Index>(v))];
        const Vec3 p = mesh.position(VertexId{static_cast<Index>(v)});
        const float w = std::exp(-9.0f * lengthSquared(field.o[v] - p) / (s * s));
        g.pos[node] += field.o[v] * w;
        g.normal[node] += field.normal[v] * w;
        weight[node] += w;
    }
    for (std::size_t k = 0; k < g.pos.size(); ++k) {
        if (weight[k] > 1e-12f) {
            g.pos[k] = g.pos[k] / weight[k];
        }
        g.normal[k] = normalized(g.normal[k]);
    }

    // --- Node adjacency from the (resolved, deduplicated) lattice edges. ---
    g.adj.assign(g.pos.size(), {});
    std::vector<std::vector<Index>> nodeAdj(g.pos.size());
    for (std::size_t v = 0; v < nV; ++v) {
        if (!field.valid[v]) {
            continue;
        }
        const Index a = nodeOf[dset.find(static_cast<Index>(v))];
        for (const Index nb : latticeAdj[v]) {
            const Index b = nodeOf[dset.find(nb)];
            if (a != b) {
                nodeAdj[a].push_back(b);
            }
        }
    }

    // --- A6: dedupe + angular ordering of each node's neighbours. ---
    for (std::size_t a = 0; a < nodeAdj.size(); ++a) {
        std::sort(nodeAdj[a].begin(), nodeAdj[a].end());
        nodeAdj[a].erase(std::unique(nodeAdj[a].begin(), nodeAdj[a].end()), nodeAdj[a].end());
        Vec3 su, tv;
        coordinateSystem(g.normal[a], su, tv);
        std::sort(nodeAdj[a].begin(), nodeAdj[a].end(), [&](Index x, Index y) {
            const Vec3 vx = g.pos[x] - g.pos[a];
            const Vec3 vy = g.pos[y] - g.pos[a];
            return std::atan2(dot(tv, vx), dot(su, vx)) > std::atan2(dot(tv, vy), dot(su, vy));
        });
        for (const Index b : nodeAdj[a]) {
            g.adj[a].push_back(Link{b, 0});
        }
    }
    return g;
}

// --- Stage B: trace faces by walking the angularly-ordered graph. ---------
// From a start half-edge, repeatedly step to the neighbour and take the next
// edge in that neighbour's angular fan after the back-edge ("turn left"),
// until returning to the start. A closed loop of exactly `targetSize` with no
// already-used half-edge is a face; its half-edges are then marked used so the
// result is a manifold cover. Quads (size 4) are extracted first.
Mesh extractFaces(const Graph& g) {
    const std::size_t nN = g.pos.size();
    std::vector<std::vector<std::uint8_t>> used(nN);
    for (std::size_t i = 0; i < nN; ++i) {
        used[i].assign(g.adj[i].size(), 0);
    }
    const auto valence = [&](Index n) { return static_cast<int>(g.adj[static_cast<std::size_t>(n)].size()); };
    const auto backEdge = [&](Index from, Index to) {
        const auto& a = g.adj[static_cast<std::size_t>(to)];
        for (int k = 0; k < static_cast<int>(a.size()); ++k) {
            if (a[static_cast<std::size_t>(k)].id == from) {
                return k;
            }
        }
        return -1;
    };

    std::vector<std::vector<Index>> faces;
    for (const int targetSize : {4, 3, 5, 6}) {
        for (Index start = 0; start < static_cast<Index>(nN); ++start) {
            for (int startIdx = 0; startIdx < valence(start); ++startIdx) {
                if (used[start][static_cast<std::size_t>(startIdx)]) {
                    continue;
                }
                std::vector<std::pair<Index, int>> path;
                Index cur = start;
                int ci = startIdx;
                bool ok = false;
                while (true) {
                    if (used[static_cast<std::size_t>(cur)][static_cast<std::size_t>(ci)]) {
                        break;
                    }
                    if (static_cast<int>(path.size()) + 1 > targetSize) {
                        break;
                    }
                    path.emplace_back(cur, ci);
                    const Index next = g.adj[static_cast<std::size_t>(cur)][static_cast<std::size_t>(ci)].id;
                    const int bi = backEdge(next, cur);
                    if (bi < 0 || valence(next) <= 1) {
                        break;
                    }
                    ci = (bi + 1) % valence(next);
                    cur = next;
                    if (cur == start) {
                        ok = static_cast<int>(path.size()) == targetSize;
                        break;
                    }
                }
                if (!ok) {
                    continue;
                }
                std::vector<Index> face;
                face.reserve(path.size());
                for (const auto& [n, idx] : path) {
                    face.push_back(n);
                }
                // Reject degenerate faces (a repeated vertex).
                std::vector<Index> sorted = face;
                std::sort(sorted.begin(), sorted.end());
                if (std::adjacent_find(sorted.begin(), sorted.end()) != sorted.end()) {
                    continue;
                }
                for (const auto& [n, idx] : path) {
                    used[static_cast<std::size_t>(n)][static_cast<std::size_t>(idx)] = 1;
                }
                faces.push_back(std::move(face));
            }
        }
    }
    return Mesh::fromIndexed(g.pos, faces);
}

}  // namespace

Mesh extractQuadMesh(const Mesh& mesh, const PositionField& field) {
    const Graph g = buildCollapsedGraph(mesh, field);
    return extractFaces(g);
}

CollapsedGraphStats debugCollapse(const Mesh& mesh, const PositionField& field) {
    const Graph g = buildCollapsedGraph(mesh, field);
    std::size_t edges = 0;
    for (const auto& a : g.adj) {
        edges += a.size();
    }
    return {g.pos.size(), edges / 2};
}

}  // namespace cyber::remesh
