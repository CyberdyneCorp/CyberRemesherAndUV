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
#include <deque>
#include <map>
#include <numeric>
#include <queue>
#include <set>
#include <unordered_map>
#include <utility>
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

// --- Integer-parametrization Stage-2 helpers (clean-room from QuadriFlow) -----

// Rotate an integer 2-D vector by amount*90 degrees (QuadriFlow rshift90).
Int2 rshift90i(Int2 v, int amount) {
    if (amount & 1) {
        v = Int2{-v.y, v.x};
    }
    if (amount >= 2) {
        v = Int2{-v.x, -v.y};
    }
    return v;
}

// 4-RoSy orientation transition index (QuadriFlow compat_orientation_extrinsic_
// index_4): returns (a, b) with a in {0,1}, b in {0,1,2,3}; the rotation from q1's
// frame to q0's is (a - b + 4) % 4.
std::pair<int, int> orientIndex(Vec3 q0, Vec3 n0, Vec3 q1, Vec3 n1) {
    const std::array<Vec3, 2> a{q0, cross(n0, q0)};
    const std::array<Vec3, 2> b{q1, cross(n1, q1)};
    float best = -1e30f;
    int ba = 0, bb = 0;
    for (int i = 0; i < 2; ++i) {
        for (int j = 0; j < 2; ++j) {
            const float s = std::fabs(dot(a[static_cast<std::size_t>(i)], b[static_cast<std::size_t>(j)]));
            if (s > best) {
                best = s;
                ba = i;
                bb = j;
            }
        }
    }
    if (dot(a[static_cast<std::size_t>(ba)], b[static_cast<std::size_t>(bb)]) < 0.0f) {
        bb += 2;
    }
    return {ba, bb};
}

// Union-find with a relative 90-degree orientation per node (QuadriFlow
// DisajointOrientTree): globally aligns the per-face edge orientations so a
// shared edge maps consistently between its two faces.
struct OrientTree {
    std::vector<std::pair<int, int>> parent;  // (parent, orientation to parent, mod 4)
    std::vector<int> rank;
    explicit OrientTree(int nn) : parent(static_cast<std::size_t>(nn)), rank(static_cast<std::size_t>(nn), 1) {
        for (int i = 0; i < nn; ++i) {
            parent[static_cast<std::size_t>(i)] = {i, 0};
        }
    }
    int root(int j) {
        auto& pj = parent[static_cast<std::size_t>(j)];
        if (j == pj.first) {
            return j;
        }
        const int k = root(pj.first);
        pj.second = (pj.second + parent[static_cast<std::size_t>(pj.first)].second) % 4;
        pj.first = k;
        return k;
    }
    int orient(int j) {
        const auto& pj = parent[static_cast<std::size_t>(j)];
        return j == pj.first ? pj.second : (pj.second + orient(pj.first)) % 4;
    }
    void merge(int v0, int v1, int o0, int o1) {
        const int p0 = root(v0), p1 = root(v1);
        if (p0 == p1) {
            return;
        }
        const int op0 = orient(v0), op1 = orient(v1);
        if (rank[static_cast<std::size_t>(p1)] < rank[static_cast<std::size_t>(p0)]) {
            rank[static_cast<std::size_t>(p0)] += rank[static_cast<std::size_t>(p1)];
            parent[static_cast<std::size_t>(p1)] = {p0, (o1 - o0 + op0 - op1 + 8) % 4};
        } else {
            rank[static_cast<std::size_t>(p1)] += rank[static_cast<std::size_t>(p0)];
            parent[static_cast<std::size_t>(p0)] = {p1, (o0 - o1 + op1 - op0 + 8) % 4};
        }
    }
};

// Min-cost max-flow (SPFA successive shortest paths). The reusable engine for
// the integer-parametrization Stage-2 solve (docs/integer-parametrization-plan.md):
// the integer optimization is a min-cost flow that balances per-face coordinate
// divergences by adjusting per-edge integer jumps. Clean-room; small graphs
// (~1e4 nodes for our corpus), so SPFA is fine.
class MinCostFlow {
public:
    void init(int nodes) {
        n_ = nodes;
        adj_.assign(static_cast<std::size_t>(nodes), {});
        edges_.clear();
    }
    // Directed arc u->v with capacity cap and per-unit cost. Returns the index of
    // the forward arc (its flow is readable afterwards via flow()).
    int addEdge(int u, int v, int cap, int cost) {
        const int id = static_cast<int>(edges_.size());
        adj_[static_cast<std::size_t>(u)].push_back(id);
        edges_.push_back(Edge{v, cap, 0, cost});
        adj_[static_cast<std::size_t>(v)].push_back(id + 1);
        edges_.push_back(Edge{u, 0, 0, -cost});
        return id;
    }
    // Push max flow from s to t at minimum cost. Returns {flow, cost}.
    std::pair<int, int> solve(int s, int t) {
        int totalFlow = 0, totalCost = 0;
        while (true) {
            std::vector<int> dist(static_cast<std::size_t>(n_), kInf);
            std::vector<int> inQueue(static_cast<std::size_t>(n_), 0);
            std::vector<int> prevEdge(static_cast<std::size_t>(n_), -1);
            dist[static_cast<std::size_t>(s)] = 0;
            std::vector<int> queue{s};
            inQueue[static_cast<std::size_t>(s)] = 1;
            std::size_t head = 0;
            while (head < queue.size()) {
                const int u = queue[head++];
                inQueue[static_cast<std::size_t>(u)] = 0;
                for (const int id : adj_[static_cast<std::size_t>(u)]) {
                    const Edge& e = edges_[static_cast<std::size_t>(id)];
                    if (e.cap - e.flow <= 0 || dist[static_cast<std::size_t>(u)] == kInf) {
                        continue;
                    }
                    const int nd = dist[static_cast<std::size_t>(u)] + e.cost;
                    if (nd < dist[static_cast<std::size_t>(e.to)]) {
                        dist[static_cast<std::size_t>(e.to)] = nd;
                        prevEdge[static_cast<std::size_t>(e.to)] = id;
                        if (!inQueue[static_cast<std::size_t>(e.to)]) {
                            inQueue[static_cast<std::size_t>(e.to)] = 1;
                            queue.push_back(e.to);
                        }
                    }
                }
            }
            if (dist[static_cast<std::size_t>(t)] == kInf) {
                break;  // no augmenting path
            }
            int push = kInf;
            for (int v = t; v != s; ) {
                const Edge& e = edges_[static_cast<std::size_t>(prevEdge[static_cast<std::size_t>(v)])];
                push = std::min(push, e.cap - e.flow);
                v = edges_[static_cast<std::size_t>(prevEdge[static_cast<std::size_t>(v)] ^ 1)].to;
            }
            for (int v = t; v != s; ) {
                const int id = prevEdge[static_cast<std::size_t>(v)];
                edges_[static_cast<std::size_t>(id)].flow += push;
                edges_[static_cast<std::size_t>(id ^ 1)].flow -= push;
                v = edges_[static_cast<std::size_t>(id ^ 1)].to;
            }
            totalFlow += push;
            totalCost += push * dist[static_cast<std::size_t>(t)];
        }
        return {totalFlow, totalCost};
    }
    [[nodiscard]] int flow(int forwardEdgeId) const {
        return edges_[static_cast<std::size_t>(forwardEdgeId)].flow;
    }

private:
    struct Edge {
        int to, cap, flow, cost;
    };
    static constexpr int kInf = 1 << 30;
    int n_ = 0;
    std::vector<Edge> edges_;
    std::vector<std::vector<int>> adj_;
};

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

// --- Graph cleanup (Instant Meshes extract_graph "Step 5"). ----------------

// Snap/merge/project sweep: for every 2-path i-j-k whose triangle altitude from
// i onto edge (j,k) is below `thresh`, either merge i with a near-coincident
// endpoint or slide i onto edge (j,k) — repairing the sliver triangles an
// imperfect position field leaves. Returns whether anything changed.
bool snapMergeProject(std::vector<Vec3>& pos, std::vector<Vec3>& normal,
                      std::vector<std::set<int>>& adj, float thresh) {
    const auto len = [&](int a, int b) {
        return length(pos[static_cast<std::size_t>(a)] - pos[static_cast<std::size_t>(b)]);
    };
    const auto height = [&](int i, int j, int k) {  // altitude from i onto (j,k), or -1
        const float a = len(j, k), b = len(i, j), c = len(i, k);
        if (a <= std::fmax(b, c)) {
            return -1.0f;  // (j,k) not the longest side -> i is not the sliver tip
        }
        const float sp = (a + b + c) * 0.5f;
        const float area2 = sp * (sp - a) * (sp - b) * (sp - c);
        return area2 > 0.0f ? 2.0f * std::sqrt(area2) / a : -1.0f;
    };

    struct Cand {
        float h;
        int i, j, k;
    };
    std::vector<Cand> cands;
    for (int i = 0; i < static_cast<int>(adj.size()); ++i) {
        for (const int j : adj[static_cast<std::size_t>(i)]) {
            for (const int k : adj[static_cast<std::size_t>(j)]) {
                if (k == i) {
                    continue;
                }
                const float h = height(i, j, k);
                if (h >= 0.0f && h < thresh) {
                    cands.push_back({h, i, j, k});
                }
            }
        }
    }
    std::sort(cands.begin(), cands.end(), [](const Cand& x, const Cand& y) { return x.h < y.h; });

    bool changed = false;
    for (const Cand& cd : cands) {
        const int i = cd.i, j = cd.j, k = cd.k;
        if (!adj[static_cast<std::size_t>(i)].count(j) || !adj[static_cast<std::size_t>(j)].count(k)) {
            continue;
        }
        if (height(i, j, k) != cd.h) {
            continue;  // graph changed around this triple -> stale
        }
        const bool hasKI = adj[static_cast<std::size_t>(k)].count(i) > 0;
        const float b = len(i, j), c = len(i, k);
        if (b < thresh || c < thresh) {  // MERGE: dissolve the near endpoint into i
            const int mid = b < thresh ? j : k;
            pos[static_cast<std::size_t>(i)] =
                (pos[static_cast<std::size_t>(i)] + pos[static_cast<std::size_t>(mid)]) * 0.5f;
            normal[static_cast<std::size_t>(i)] =
                (normal[static_cast<std::size_t>(i)] + normal[static_cast<std::size_t>(mid)]) * 0.5f;
            for (const int n : adj[static_cast<std::size_t>(mid)]) {
                if (n == i) {
                    continue;
                }
                adj[static_cast<std::size_t>(n)].erase(mid);
                adj[static_cast<std::size_t>(n)].insert(i);
                adj[static_cast<std::size_t>(i)].insert(n);
            }
            adj[static_cast<std::size_t>(i)].erase(i);
            adj[static_cast<std::size_t>(i)].erase(mid);
            adj[static_cast<std::size_t>(mid)].clear();
        } else {  // PROJECT: slide i onto edge (j,k), splitting it
            pos[static_cast<std::size_t>(i)] =
                (pos[static_cast<std::size_t>(j)] + pos[static_cast<std::size_t>(k)]) * 0.5f;
            normal[static_cast<std::size_t>(i)] =
                normalized(normal[static_cast<std::size_t>(j)] + normal[static_cast<std::size_t>(k)]);
            adj[static_cast<std::size_t>(j)].erase(k);
            adj[static_cast<std::size_t>(k)].erase(j);
            if (!hasKI) {
                adj[static_cast<std::size_t>(i)].insert(k);
                adj[static_cast<std::size_t>(k)].insert(i);
            }
        }
        changed = true;
    }
    return changed;
}

// Diagonal removal: delete each edge (i,j) that is the shared diagonal of two
// triangles (exactly two common neighbours) whose four outer edges make it look
// like a near-square, fusing the two triangles into one quad. Returns whether
// anything changed.
bool removeDiagonals(const std::vector<Vec3>& pos, std::vector<std::set<int>>& adj) {
    const auto len = [&](int a, int b) {
        return length(pos[static_cast<std::size_t>(a)] - pos[static_cast<std::size_t>(b)]);
    };
    struct DCand {
        float score;
        int i, j;
    };
    std::vector<DCand> cands;
    for (int i = 0; i < static_cast<int>(adj.size()); ++i) {
        for (const int j : adj[static_cast<std::size_t>(i)]) {
            if (j <= i) {
                continue;  // each undirected edge once
            }
            int nTris = 0;
            float outer = 0.0f;
            for (const int k : adj[static_cast<std::size_t>(i)]) {
                if (adj[static_cast<std::size_t>(j)].count(k)) {
                    ++nTris;
                    outer += len(k, i) + len(k, j);
                }
            }
            if (nTris != 2) {
                continue;
            }
            const float diag = len(i, j);
            const float expDiag = outer / 4.0f * std::sqrt(2.0f);
            const float score = std::fabs(diag - expDiag) / std::fmax(1e-9f, std::fmin(diag, expDiag));
            cands.push_back({score, i, j});
        }
    }
    std::sort(cands.begin(), cands.end(), [](const DCand& x, const DCand& y) { return x.score < y.score; });

    bool changed = false;
    for (const DCand& dc : cands) {
        const int i = dc.i, j = dc.j;
        if (!adj[static_cast<std::size_t>(i)].count(j)) {
            continue;
        }
        int nTris = 0;
        for (const int k : adj[static_cast<std::size_t>(i)]) {
            if (adj[static_cast<std::size_t>(j)].count(k)) {
                ++nTris;
            }
        }
        if (nTris != 2) {
            continue;  // a neighbouring removal already changed the diamond
        }
        adj[static_cast<std::size_t>(i)].erase(j);
        adj[static_cast<std::size_t>(j)].erase(i);
        changed = true;
    }
    return changed;
}

// Repairs the collapsed lattice graph before the face walk: snap/merge/project
// to a fixpoint, then one diagonal-removal pass, repeated until stable. This
// lets the extraction tolerate an imperfect position field, pushing node valence
// back toward 4 and fusing stray triangles into quads.
void removeUnnecessaryEdges(std::vector<Vec3>& pos, std::vector<Vec3>& normal,
                            std::vector<std::set<int>>& adj, float s) {
    const float thresh = 0.3f * s;
    for (int guard = 0; guard < 64; ++guard) {
        bool changed = false;
        while (snapMergeProject(pos, normal, adj, thresh)) {
            changed = true;
        }
        if (removeDiagonals(pos, adj)) {
            changed = true;
        }
        if (!changed) {
            break;
        }
    }
}

Graph buildCollapsedGraph(const Mesh& mesh, const PositionField& field) {
    const float s = field.spacing;
    const std::size_t nV = field.size();

    // Local lattice spacing: the base spacing scaled by the per-vertex density
    // multiplier (1 on a uniform mesh, so these reduce to the global s; on an
    // adaptive mesh the cell size follows local density so coarse regions are not
    // over-merged). Per-edge uses the two endpoints' average.
    const auto nodeS = [&](std::size_t i) {
        return field.scale.empty() ? s : s * field.scale[i];
    };
    const auto edgeS = [&](std::size_t i, std::size_t j) {
        return field.scale.empty() ? s : s * 0.5f * (field.scale[i] + field.scale[j]);
    };

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
        const float es = edgeS(i, j);
        const PosCompat pc =
            compatPosition(mesh.position(va), field.normal[i], q0, field.o[i], mesh.position(vb),
                           field.normal[j], q1, field.o[j], es, 1.0f / es);
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
        const float sv = nodeS(v);
        const float w = std::exp(-9.0f * lengthSquared(field.o[v] - p) / (sv * sv));
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

    // --- Node adjacency (as sets) from the resolved lattice edges. ---
    std::vector<std::set<int>> nodeAdj(g.pos.size());
    for (std::size_t v = 0; v < nV; ++v) {
        if (!field.valid[v]) {
            continue;
        }
        const int a = static_cast<int>(nodeOf[dset.find(static_cast<Index>(v))]);
        for (const Index nb : latticeAdj[v]) {
            const int b = static_cast<int>(nodeOf[dset.find(nb)]);
            if (a != b) {
                nodeAdj[static_cast<std::size_t>(a)].insert(b);
                nodeAdj[static_cast<std::size_t>(b)].insert(a);
            }
        }
    }

    // --- A4b: recover lattice edges the per-vertex offsets missed. Node
    // centroids are far cleaner than the noisy per-vertex field, so re-test
    // every cross-node mesh edge by centroid distance: cells one spacing apart
    // are lattice neighbours, cells ~s*sqrt(2) apart are quad diagonals. This
    // restores the ~6% of unit edges that per-vertex field noise misclassified
    // as diagonal/far, raising graph valence toward the grid ideal of 4. ---
    {
        for (Index ei = 0; ei < mesh.edgeCapacity(); ++ei) {
            const EdgeId e{ei};
            if (!mesh.isAlive(e)) {
                continue;
            }
            const auto [va, vb] = mesh.edgeVertices(e);
            if (!field.valid[va.value] || !field.valid[vb.value]) {
                continue;
            }
            const int a = static_cast<int>(nodeOf[dset.find(va.value)]);
            const int b = static_cast<int>(nodeOf[dset.find(vb.value)]);
            if (a == b || nodeAdj[static_cast<std::size_t>(a)].count(b)) {
                continue;
            }
            // Local thresholds: same cell below 0.55s, quad diagonal above 1.30s
            // (a true diagonal is s*sqrt2 = 1.414s), scaled by the edge's local
            // spacing so recovery works on adaptive as well as uniform meshes.
            const float es = edgeS(va.value, vb.value);
            const float lo2 = (0.55f * es) * (0.55f * es);
            const float hi2 = (1.30f * es) * (1.30f * es);
            const float d2 = lengthSquared(g.pos[static_cast<std::size_t>(a)] -
                                           g.pos[static_cast<std::size_t>(b)]);
            if (d2 >= lo2 && d2 <= hi2) {
                nodeAdj[static_cast<std::size_t>(a)].insert(b);
                nodeAdj[static_cast<std::size_t>(b)].insert(a);
            }
        }
    }

    // --- A5: graph cleanup (snap/merge/project + diagonal removal). ---
    removeUnnecessaryEdges(g.pos, g.normal, nodeAdj, s);

    // --- A6: angular ordering of each node's neighbours into the walk graph. ---
    g.adj.assign(g.pos.size(), {});
    for (std::size_t a = 0; a < nodeAdj.size(); ++a) {
        std::vector<Index> nbrs(nodeAdj[a].begin(), nodeAdj[a].end());
        Vec3 su, tv;
        coordinateSystem(g.normal[a], su, tv);
        std::sort(nbrs.begin(), nbrs.end(), [&](Index x, Index y) {
            const Vec3 vx = g.pos[x] - g.pos[a];
            const Vec3 vy = g.pos[y] - g.pos[a];
            return std::atan2(dot(tv, vx), dot(su, vx)) > std::atan2(dot(tv, vy), dot(su, vy));
        });
        for (const Index b : nbrs) {
            g.adj[a].push_back(Link{b, 0});
        }
    }
    return g;
}

// --- Stage B: trace faces as orbits of the rotation system. ----------------
// The angularly-ordered adjacency is a rotation system; each face is an orbit
// of phi = sigma . alpha over the directed edges ("darts"): from a dart u->v,
// reverse to v->u and take the CCW-successor neighbour of u around v. Every dart
// belongs to exactly ONE orbit, so this covers the WHOLE graph — unlike a
// target-size turn-left walk, which abandons darts that do not close a 4-cycle
// and so leaves large holes on curved / singular surfaces. Orbits of size 3-11
// become faces (quads / tris / small n-gons filling holes around singularities);
// larger orbits are open-boundary loops and are dropped.
// Greedy-cut an orbit loop into quads (Instant Meshes fill_face): repeatedly cut
// off the four consecutive corners whose quad angles are closest to 90 degrees,
// removing that quad's two interior corners (the cut edge joins its endpoints),
// until four or fewer remain. Uses only the orbit's own vertices — no centroid —
// so hole quads are well-shaped instead of the pie slices a centroid fan leaves.
void fillFace(std::vector<Index> loop, const std::vector<Vec3>& pos,
              std::vector<std::vector<Index>>& faces) {
    const auto angle = [&](Index prev, Index cur, Index next) {
        const Vec3 a = normalized(pos[prev] - pos[cur]);
        const Vec3 b = normalized(pos[next] - pos[cur]);
        return std::acos(std::clamp(dot(a, b), -1.0f, 1.0f));
    };
    while (loop.size() > 4) {
        const std::size_t n = loop.size();
        float best = 1e30f;
        std::size_t bestI = 0;
        for (std::size_t i = 0; i < n; ++i) {
            const Index a = loop[i], b = loop[(i + 1) % n], c = loop[(i + 2) % n],
                        d = loop[(i + 3) % n];
            const float score = std::fabs(angle(d, a, b) - kPi / 2.0f) +
                                std::fabs(angle(a, b, c) - kPi / 2.0f) +
                                std::fabs(angle(b, c, d) - kPi / 2.0f) +
                                std::fabs(angle(c, d, a) - kPi / 2.0f);
            if (score < best) {
                best = score;
                bestI = i;
            }
        }
        faces.push_back({loop[bestI], loop[(bestI + 1) % n], loop[(bestI + 2) % n],
                         loop[(bestI + 3) % n]});
        const std::size_t r1 = (bestI + 1) % n, r2 = (bestI + 2) % n;
        std::vector<Index> rest;
        for (std::size_t k = 0; k < n; ++k) {
            if (k != r1 && k != r2) {
                rest.push_back(loop[k]);
            }
        }
        loop = std::move(rest);
    }
    if (loop.size() >= 3) {
        faces.push_back(std::move(loop));
    }
}

Mesh extractFaces(const Graph& g, bool closedInput) {
    const std::size_t nN = g.pos.size();

    // Darts (directed edges), indexed for O(log n) reverse/lookup; ringPos[d] is
    // the index of dst within src's CCW neighbour ring.
    std::map<std::pair<Index, Index>, int> dartId;
    std::vector<Index> src, dst;
    std::vector<int> ringPos;
    for (Index u = 0; u < static_cast<Index>(nN); ++u) {
        const auto& ring = g.adj[u];
        for (int k = 0; k < static_cast<int>(ring.size()); ++k) {
            dartId[{u, ring[static_cast<std::size_t>(k)].id}] = static_cast<int>(src.size());
            src.push_back(u);
            dst.push_back(ring[static_cast<std::size_t>(k)].id);
            ringPos.push_back(k);
        }
    }
    const std::size_t dartCount = src.size();

    const auto nextDart = [&](int dart) -> int {
        const Index u = src[static_cast<std::size_t>(dart)];
        const Index v = dst[static_cast<std::size_t>(dart)];
        const auto rev = dartId.find({v, u});
        if (rev == dartId.end()) {
            return -1;
        }
        const auto& ring = g.adj[v];
        const int deg = static_cast<int>(ring.size());
        const int i = ringPos[static_cast<std::size_t>(rev->second)];
        const Index w = ring[static_cast<std::size_t>((i + 1) % deg)].id;
        const auto jt = dartId.find({v, w});
        return jt == dartId.end() ? -1 : jt->second;
    };

    // On a closed input every orbit is a real face, so fill large hole orbits
    // (up to a pathological cap) instead of dropping them as boundaries; on an
    // open input, orbits at/above the small threshold are the genuine mesh
    // boundary and stay open. This keeps closed models watertight (fewer holes
    // for the downstream fill to botch) without inventing faces across real holes.
    const std::size_t kBoundaryLoop = closedInput ? 64 : 12;
    std::vector<char> used(dartCount, 0);
    std::vector<std::vector<Index>> faces;
    for (int start = 0; start < static_cast<int>(dartCount); ++start) {
        if (used[static_cast<std::size_t>(start)]) {
            continue;
        }
        std::vector<Index> loop;
        int dart = start;
        bool ok = true;
        for (std::size_t guard = 0; guard <= dartCount; ++guard) {
            if (dart < 0 || used[static_cast<std::size_t>(dart)]) {
                ok = dart == start;
                break;
            }
            used[static_cast<std::size_t>(dart)] = 1;
            loop.push_back(src[static_cast<std::size_t>(dart)]);
            dart = nextDart(dart);
            if (dart == start) {
                break;  // orbit closed
            }
        }
        const std::size_t n = loop.size();
        if (!ok || n < 3 || n >= kBoundaryLoop) {
            continue;
        }
        std::vector<Index> sorted = loop;
        std::sort(sorted.begin(), sorted.end());
        if (std::adjacent_find(sorted.begin(), sorted.end()) != sorted.end()) {
            continue;  // repeated node -> degenerate orbit
        }
        if (n == 3 || n == 4) {
            faces.push_back(std::move(loop));
            continue;
        }
        // Greedy-cut a hole orbit (5..11) into well-shaped quads using only its
        // own vertices — no centroid, so no pie-slice slivers (see fillFace).
        fillFace(std::move(loop), g.pos, faces);
    }
    return Mesh::fromIndexed(g.pos, faces);
}

}  // namespace

Mesh extractQuadMesh(const Mesh& mesh, const PositionField& field) {
    const Graph g = buildCollapsedGraph(mesh, field);
    bool closed = true;
    for (Index ei = 0; ei < mesh.edgeCapacity(); ++ei) {
        const EdgeId e{ei};
        if (mesh.isAlive(e) && mesh.isBoundaryEdge(e)) {
            closed = false;
            break;
        }
    }
    return extractFaces(g, closed);
}

namespace {

// IQuadrangulator wrapper: build the position field on the (isotropic, at ~edge
// length) triangle mesh, extract a quad mesh, and replace the input in place.
class InstantMeshesQuadrangulator final : public IQuadrangulator {
public:
    explicit InstantMeshesQuadrangulator(int iterations) : m_iterations(iterations) {}

    Outcome quadrangulate(Mesh& mesh, float targetEdgeLength, ProgressSink* progress,
                          const CancelToken* cancel) override {
        if (cancel != nullptr && cancel->isCancelled()) {
            return {.success = false, .cancelled = true, .failureReason = "cancelled"};
        }
        const PositionField field = computePositionField(mesh, targetEdgeLength, m_iterations);
        Mesh quads = extractQuadMesh(mesh, field);
        if (quads.faceCount() == 0) {
            return {.success = false, .cancelled = false,
                    .failureReason = "position-field extraction produced no faces"};
        }
        mesh = std::move(quads);
        if (progress != nullptr) {
            progress->report(1.0f, "quadrangulate");
        }
        return {.success = true, .cancelled = false, .failureReason = {}};
    }

    [[nodiscard]] std::string name() const override { return "instant-meshes"; }

private:
    int m_iterations;
};

}  // namespace

std::unique_ptr<IQuadrangulator> makeInstantMeshesQuadrangulator(int iterations) {
    return std::make_unique<InstantMeshesQuadrangulator>(iterations);
}

CollapsedGraphStats debugCollapse(const Mesh& mesh, const PositionField& field) {
    const Graph g = buildCollapsedGraph(mesh, field);
    std::size_t edges = 0;
    for (const auto& a : g.adj) {
        edges += a.size();
    }
    return {g.pos.size(), edges / 2};
}

std::size_t debugPositionSingularities(const Mesh& mesh, const PositionField& field) {
    // QuadriFlow's ComputePositionSingularities: for each triangle, jointly align
    // the three crosses (best of 4^3 rotations, maximizing the minimum pairwise
    // dot), then sum the three edges' integer position diffs in that common frame.
    // A nonzero sum is a genuine position singularity — the residual the Stage-2
    // integer solve cancels. Cleaner than pairwise holonomy (the ±1 cell-boundary
    // noise cancels within a face's joint alignment).
    std::size_t singular = 0;
    for (Index fi = 0; fi < mesh.faceCapacity(); ++fi) {
        const FaceId f{fi};
        if (!mesh.isAlive(f) || mesh.faceSize(f) != 3) {
            continue;
        }
        const auto verts = mesh.faceVertices(f);
        if (!field.valid[verts[0].value] || !field.valid[verts[1].value] ||
            !field.valid[verts[2].value]) {
            continue;
        }
        std::array<Vec3, 3> q{}, n{}, o{}, p{};
        std::array<float, 3> sc{};
        for (std::size_t k = 0; k < 3; ++k) {
            const Index vk = verts[k].value;
            q[k] = field.q[vk];
            n[k] = field.normal[vk];
            o[k] = field.o[vk];
            p[k] = mesh.position(verts[k]);
            sc[k] = field.scale.empty() ? field.spacing : field.spacing * field.scale[vk];
        }
        // rotate90_by(q, n, r) = apply cross(n, .) r times.
        const auto rot = [](Vec3 d, Vec3 nn, int r) {
            for (int i = 0; i < (r & 3); ++i) {
                d = cross(nn, d);
            }
            return d;
        };
        int best[3] = {0, 0, 0};
        float bestDp = -2.0f;
        for (int i = 0; i < 4; ++i) {
            const Vec3 v0 = rot(q[0], n[0], i);
            for (int j = 0; j < 4; ++j) {
                const Vec3 v1 = rot(q[1], n[1], j);
                for (int k = 0; k < 4; ++k) {
                    const Vec3 v2 = rot(q[2], n[2], k);
                    const float dp = std::fmin(std::fmin(dot(v0, v1), dot(v1, v2)), dot(v2, v0));
                    if (dp > bestDp) {
                        bestDp = dp;
                        best[0] = i;
                        best[1] = j;
                        best[2] = k;
                    }
                }
            }
        }
        for (std::size_t k = 0; k < 3; ++k) {
            q[k] = rot(q[k], n[k], best[k]);
        }
        Int2 index{0, 0};
        for (std::size_t k = 0; k < 3; ++k) {
            const std::size_t kn = (k + 1) % 3;
            const float s = 0.5f * (sc[k] + sc[kn]);
            const PosCompat pc =
                compatPosition(p[k], n[k], q[k], o[k], p[kn], n[kn], q[kn], o[kn], s, 1.0f / s);
            index.x += pc.i0.x - pc.i1.x;
            index.y += pc.i0.y - pc.i1.y;
        }
        if (index.x != 0 || index.y != 0) {
            ++singular;
        }
    }
    return singular;
}

namespace {

// The corrected integer grid: triangles, half-edge opposite (e2e), per-undirected-
// edge integer jump (edgeDiff, post-flow), per-face edge ids + globally-aligned
// edge orientations, and the pre-solve position singularities. This is the state
// the min-cost flow leaves behind (QuadriFlow's post-ComputeMaxFlow state) and the
// input to subdivision + quad extraction.
struct IntegerGrid {
    std::vector<std::array<Index, 3>> tris;
    std::vector<int> e2e;
    std::vector<Int2> edgeDiff;
    std::vector<std::array<int, 3>> faceEdgeIds;
    std::vector<std::array<int, 3>> orients;
    std::set<int> singular;
    int flow = 0;
    int supply = 0;
    std::size_t residualMismatch = 0;
};

// Runs the Stage-2 integer solve and returns the corrected grid (does not modify
// the mesh). Post-solve, edge_diff is globally consistent except at the sparse
// true singularities.
IntegerGrid computeIntegerGrid(const Mesh& mesh, const PositionField& field) {
    std::size_t residualMismatch = 0;

    // 1. Triangles + half-edge opposite map (E2E), undirected edge implied later.
    std::vector<std::array<Index, 3>> tris;
    for (Index fi = 0; fi < mesh.faceCapacity(); ++fi) {
        const FaceId f{fi};
        if (!mesh.isAlive(f) || mesh.faceSize(f) != 3) {
            continue;
        }
        const auto v = mesh.faceVertices(f);
        if (field.valid[v[0].value] && field.valid[v[1].value] && field.valid[v[2].value]) {
            tris.push_back({v[0].value, v[1].value, v[2].value});
        }
    }
    const int nTri = static_cast<int>(tris.size());
    std::map<std::pair<Index, Index>, int> heMap;
    for (int t = 0; t < nTri; ++t) {
        for (int j = 0; j < 3; ++j) {
            heMap[{tris[static_cast<std::size_t>(t)][static_cast<std::size_t>(j)],
                   tris[static_cast<std::size_t>(t)][static_cast<std::size_t>((j + 1) % 3)]}] = t * 3 + j;
        }
    }
    std::vector<int> e2e(static_cast<std::size_t>(nTri) * 3, -1);
    for (int t = 0; t < nTri; ++t) {
        for (int j = 0; j < 3; ++j) {
            const auto it = heMap.find({tris[static_cast<std::size_t>(t)][static_cast<std::size_t>((j + 1) % 3)],
                                        tris[static_cast<std::size_t>(t)][static_cast<std::size_t>(j)]});
            if (it != heMap.end()) {
                e2e[static_cast<std::size_t>(t * 3 + j)] = it->second;
            }
        }
    }

    // 2. Per-face joint alignment -> pos_rank, pos_index (per edge), singularities.
    std::vector<std::array<int, 3>> posRank(static_cast<std::size_t>(nTri));
    std::vector<std::array<Int2, 3>> posIndex(static_cast<std::size_t>(nTri));
    std::set<int> singular;
    const auto rot = [](Vec3 d, Vec3 nn, int r) {
        for (int i = 0; i < (r & 3); ++i) {
            d = cross(nn, d);
        }
        return d;
    };
    for (int t = 0; t < nTri; ++t) {
        std::array<Vec3, 3> q{}, n{}, o{}, p{};
        std::array<float, 3> sc{};
        for (std::size_t k = 0; k < 3; ++k) {
            const Index vk = tris[static_cast<std::size_t>(t)][k];
            q[k] = field.q[vk];
            n[k] = field.normal[vk];
            o[k] = field.o[vk];
            p[k] = mesh.position(VertexId{vk});
            sc[k] = field.scale.empty() ? field.spacing : field.spacing * field.scale[vk];
        }
        int best[3] = {0, 0, 0};
        float bestDp = -2.0f;
        for (int i = 0; i < 4; ++i) {
            const Vec3 v0 = rot(q[0], n[0], i);
            for (int j = 0; j < 4; ++j) {
                const Vec3 v1 = rot(q[1], n[1], j);
                for (int k = 0; k < 4; ++k) {
                    const Vec3 v2 = rot(q[2], n[2], k);
                    const float dp = std::fmin(std::fmin(dot(v0, v1), dot(v1, v2)), dot(v2, v0));
                    if (dp > bestDp) {
                        bestDp = dp;
                        best[0] = i;
                        best[1] = j;
                        best[2] = k;
                    }
                }
            }
        }
        std::array<Vec3, 3> qr{};
        for (std::size_t k = 0; k < 3; ++k) {
            qr[k] = rot(q[k], n[k], best[k]);
            posRank[static_cast<std::size_t>(t)][k] = best[k];
        }
        Int2 index{0, 0};
        for (std::size_t k = 0; k < 3; ++k) {
            const std::size_t kn = (k + 1) % 3;
            const float s = 0.5f * (sc[k] + sc[kn]);
            const PosCompat pc =
                compatPosition(p[k], n[k], qr[k], o[k], p[kn], n[kn], qr[kn], o[kn], s, 1.0f / s);
            posIndex[static_cast<std::size_t>(t)][k] = Int2{pc.i0.x - pc.i1.x, pc.i0.y - pc.i1.y};
            index.x += posIndex[static_cast<std::size_t>(t)][k].x;
            index.y += posIndex[static_cast<std::size_t>(t)][k].y;
        }
        if (index.x != 0 || index.y != 0) {
            singular.insert(t);
        }
    }

    // 3. BuildEdgeInfo: undirected edge ids + edge_diff.
    std::vector<Int2> edgeDiff;
    std::vector<std::array<int, 3>> faceEdgeIds(static_cast<std::size_t>(nTri), {-1, -1, -1});
    for (int t = 0; t < nTri; ++t) {
        for (int j = 0; j < 3; ++j) {
            const int k1 = j, k2 = (j + 1) % 3;
            const Index v1 = tris[static_cast<std::size_t>(t)][static_cast<std::size_t>(k1)];
            const Index v2 = tris[static_cast<std::size_t>(t)][static_cast<std::size_t>(k2)];
            Int2 diff2;
            if (v1 > v2) {
                const Int2 neg{-posIndex[static_cast<std::size_t>(t)][static_cast<std::size_t>(k1)].x,
                               -posIndex[static_cast<std::size_t>(t)][static_cast<std::size_t>(k1)].y};
                diff2 = rshift90i(neg, posRank[static_cast<std::size_t>(t)][static_cast<std::size_t>(k2)]);
            } else {
                diff2 = rshift90i(posIndex[static_cast<std::size_t>(t)][static_cast<std::size_t>(k1)],
                                  posRank[static_cast<std::size_t>(t)][static_cast<std::size_t>(k1)]);
            }
            const int he = t * 3 + k1;
            const int opp = e2e[static_cast<std::size_t>(he)];
            if (faceEdgeIds[static_cast<std::size_t>(t)][static_cast<std::size_t>(k1)] == -1) {
                const int eid = static_cast<int>(edgeDiff.size());
                edgeDiff.push_back(diff2);
                faceEdgeIds[static_cast<std::size_t>(t)][static_cast<std::size_t>(k1)] = eid;
                if (opp != -1) {
                    faceEdgeIds[static_cast<std::size_t>(opp / 3)][static_cast<std::size_t>(opp % 3)] = eid;
                }
            } else if (!singular.count(t) && opp != -1) {
                const int eid = faceEdgeIds[static_cast<std::size_t>(opp / 3)][static_cast<std::size_t>(opp % 3)];
                edgeDiff[static_cast<std::size_t>(eid)] = diff2;
            }
        }
    }

    // 4. BuildIntegerConstraints: per-face edge orientations + global alignment.
    std::vector<std::array<int, 3>> orients(static_cast<std::size_t>(nTri));
    for (int t = 0; t < nTri; ++t) {
        const Index v0 = tris[static_cast<std::size_t>(t)][0], v1 = tris[static_cast<std::size_t>(t)][1],
                    v2 = tris[static_cast<std::size_t>(t)][2];
        const auto i1 = orientIndex(field.q[v0], field.normal[v0], field.q[v1], field.normal[v1]);
        const auto i2 = orientIndex(field.q[v0], field.normal[v0], field.q[v2], field.normal[v2]);
        const int rank1 = (i1.first - i1.second + 4) % 4;
        const int rank2 = (i2.first - i2.second + 4) % 4;
        std::array<int, 3> od{0, 0, 0};
        od[0] = (v1 < v0) ? (rank1 + 2) % 4 : 0;
        od[1] = (v2 < v1) ? (rank2 + 2) % 4 : rank1;
        od[2] = (v2 < v0) ? rank2 : 2;
        orients[static_cast<std::size_t>(t)] = od;
    }
    // E2D: undirected edge -> its (up to two) directed face-edges.
    std::vector<std::pair<int, int>> e2d(edgeDiff.size(), {-1, -1});
    for (int t = 0; t < nTri; ++t) {
        for (int j = 0; j < 3; ++j) {
            const int eid = faceEdgeIds[static_cast<std::size_t>(t)][static_cast<std::size_t>(j)];
            if (e2d[static_cast<std::size_t>(eid)].first == -1) {
                e2d[static_cast<std::size_t>(eid)].first = t * 3 + j;
            } else {
                e2d[static_cast<std::size_t>(eid)].second = t * 3 + j;
            }
        }
    }
    OrientTree tree(nTri);
    for (std::size_t e = 0; e < e2d.size(); ++e) {
        const auto& c = e2d[e];
        if (c.first == -1 || c.second == -1) {
            continue;
        }
        const int f0 = c.first / 3, f1 = c.second / 3;
        if (singular.count(f0) || singular.count(f1)) {
            continue;
        }
        const int o1 = orients[static_cast<std::size_t>(f0)][static_cast<std::size_t>(c.first % 3)];
        const int o0 = (orients[static_cast<std::size_t>(f1)][static_cast<std::size_t>(c.second % 3)] + 2) % 4;
        tree.merge(f0, f1, o0, o1);
    }
    for (const int fs : singular) {
        for (int j = 0; j < 3; ++j) {
            const auto& c = e2d[static_cast<std::size_t>(faceEdgeIds[static_cast<std::size_t>(fs)][static_cast<std::size_t>(j)])];
            if (c.first == -1 || c.second == -1) {
                continue;
            }
            const int f0 = c.first / 3, f1 = c.second / 3;
            const int o1 = orients[static_cast<std::size_t>(f0)][static_cast<std::size_t>(c.first % 3)];
            const int o0 = (orients[static_cast<std::size_t>(f1)][static_cast<std::size_t>(c.second % 3)] + 2) % 4;
            tree.merge(f0, f1, o0, o1);
        }
    }
    for (int t = 0; t < nTri; ++t) {
        const int add = tree.orient(t);
        for (int j = 0; j < 3; ++j) {
            orients[static_cast<std::size_t>(t)][static_cast<std::size_t>(j)] =
                (orients[static_cast<std::size_t>(t)][static_cast<std::size_t>(j)] + add) % 4;
        }
    }

    // Checkpoint A: the residual recomputed from edge_diff/orient must match the
    // position singularities. Also build edge_to_constraints (equation, sign) per
    // edge-component and the per-equation initial residual.
    std::vector<std::array<int, 4>> e2c(edgeDiff.size() * 2, {-1, 0, -1, 0});
    std::vector<int> initial(static_cast<std::size_t>(nTri) * 2, 0);
    for (int t = 0; t < nTri; ++t) {
        Int2 res{0, 0};
        for (int j = 0; j < 3; ++j) {
            const int eid = faceEdgeIds[static_cast<std::size_t>(t)][static_cast<std::size_t>(j)];
            const int fq = orients[static_cast<std::size_t>(t)][static_cast<std::size_t>(j)];
            const Int2 contrib = rshift90i(edgeDiff[static_cast<std::size_t>(eid)], fq);
            res.x += contrib.x;
            res.y += contrib.y;
            // Signed component refs: rshift90((eid*2+1, eid*2+2), fq).
            const Int2 idx = rshift90i(Int2{eid * 2 + 1, eid * 2 + 2}, fq);
            const int comp[2] = {idx.x, idx.y};
            for (int k = 0; k < 2; ++k) {
                const int l = std::abs(comp[k]);
                const int s = comp[k] / l;
                const int ind = l - 1;
                const int eq = t * 2 + k;
                if (e2c[static_cast<std::size_t>(ind)][0] == -1) {
                    e2c[static_cast<std::size_t>(ind)][0] = eq;
                    e2c[static_cast<std::size_t>(ind)][1] = s;
                } else {
                    e2c[static_cast<std::size_t>(ind)][2] = eq;
                    e2c[static_cast<std::size_t>(ind)][3] = s;
                }
                const int diffVal = (ind % 2 == 0) ? edgeDiff[static_cast<std::size_t>(ind / 2)].x
                                                   : edgeDiff[static_cast<std::size_t>(ind / 2)].y;
                initial[static_cast<std::size_t>(eq)] += s * diffVal;
            }
        }
        const bool isSing = (res.x != 0 || res.y != 0);
        if (isSing != (singular.count(t) != 0)) {
            ++residualMismatch;
        }
    }

    // 5. Flow: nodes = equations + source + sink. Route positive residuals to
    // negative via edge-component arcs (opposite-sign pairs). Cost-1 variable
    // arcs minimize total edge_diff change.
    const int nEq = nTri * 2;
    const int source = nEq, sink = nEq + 1;
    MinCostFlow flow;
    flow.init(nEq + 2);
    int supply = 0;
    for (int e = 0; e < nEq; ++e) {
        if (initial[static_cast<std::size_t>(e)] > 0) {
            flow.addEdge(source, e, initial[static_cast<std::size_t>(e)], 0);
            supply += initial[static_cast<std::size_t>(e)];
        } else if (initial[static_cast<std::size_t>(e)] < 0) {
            flow.addEdge(e, sink, -initial[static_cast<std::size_t>(e)], 0);
        }
    }
    struct VarArc {
        int edgeId, comp, arcFwd;  // flow eq(+)->eq(-) decreases edge_diff[edgeId][comp]
    };
    std::vector<VarArc> varArcs;
    for (std::size_t ind = 0; ind < e2c.size(); ++ind) {
        const auto& c = e2c[ind];
        if (c[0] == -1 || c[2] == -1 || c[1] != -c[3]) {
            continue;  // need two equations with opposite signs
        }
        int vplus = c[0], vminus = c[2];  // the +sign equation and the -sign one
        if (c[1] < 0) {
            std::swap(vplus, vminus);
        }
        const int fwd = flow.addEdge(vplus, vminus, 1 << 20, 1);
        varArcs.push_back(VarArc{static_cast<int>(ind) / 2, static_cast<int>(ind) % 2, fwd});
    }
    const auto res = flow.solve(source, sink);

    // Apply: flow eq(+)->eq(-) on a variable arc decreases that edge_diff component.
    for (const VarArc& va : varArcs) {
        const int f = flow.flow(va.arcFwd);
        if (f != 0) {
            if (va.comp == 0) {
                edgeDiff[static_cast<std::size_t>(va.edgeId)].x -= f;
            } else {
                edgeDiff[static_cast<std::size_t>(va.edgeId)].y -= f;
            }
        }
    }

    IntegerGrid g;
    g.tris = std::move(tris);
    g.e2e = std::move(e2e);
    g.edgeDiff = std::move(edgeDiff);
    g.faceEdgeIds = std::move(faceEdgeIds);
    g.orients = std::move(orients);
    g.singular = std::move(singular);
    g.flow = res.first;
    g.supply = supply;
    g.residualMismatch = residualMismatch;
    return g;
}

// Per-face residual of the (possibly corrected) grid: faces whose three aligned
// edge diffs do not sum to zero. Pre-solve == position singularities; post-solve
// == the sparse true singularities that survive the flow.
std::size_t countResidualSingularities(const IntegerGrid& g) {
    std::size_t singular = 0;
    for (std::size_t t = 0; t < g.tris.size(); ++t) {
        Int2 r{0, 0};
        for (int j = 0; j < 3; ++j) {
            const Int2 contrib = rshift90i(g.edgeDiff[static_cast<std::size_t>(g.faceEdgeIds[t][static_cast<std::size_t>(j)])],
                                           g.orients[t][static_cast<std::size_t>(j)]);
            r.x += contrib.x;
            r.y += contrib.y;
        }
        if (r.x != 0 || r.y != 0) {
            ++singular;
        }
    }
    return singular;
}

// --- Phase 5: multi-resolution flip-repair layer (QuadriFlow FixFlipHierarchy) --
// The single-level zero-diff collapse quantizes a clean integer grid, splitting /
// over-merging grid vertices into spurious val3/val5 dipoles (~9× the true field
// singularities). QuadriFlow removes these by coarsening the edge graph to a
// fixpoint — a neighbour-independent set of zero-diff edges contracts per pass,
// with singular faces as barriers — then repairing "flipped" (negative parametric
// area) cells on the coarsest level, mutating only edge_diff. Clean-room port from
// QuadriFlow (MIT): DownsampleEdgeGraph (hierarchy.cpp:417), PropagateEdge (:1076),
// UpdateGraphValue (:410). fixFlip / checkShrink land in Milestone 5b.

using Int2Pair = std::array<int, 2>;  // an undirected edge's <= 2 incident faces

// Signed parametric area of a face's grid cell (QuadriFlow Area, hierarchy.cpp:950):
// the z of the cross product of its first two rotated edge diffs. < 0 is a flipped cell.
long faceArea(const std::vector<std::array<int, 3>>& f2e, const std::vector<std::array<int, 3>>& fq,
              const std::vector<Int2>& diff, int f) {
    const auto uz = [](int x) { return static_cast<std::size_t>(x); };
    const Int2 d0 = rshift90i(diff[uz(f2e[uz(f)][0])], fq[uz(f)][0]);
    const Int2 d1 = rshift90i(diff[uz(f2e[uz(f)][1])], fq[uz(f)][1]);
    return static_cast<long>(d0.x) * d1.y - static_cast<long>(d0.y) * d1.x;
}

std::size_t countFlipped(const IntegerGrid& g) {
    std::size_t n = 0;
    for (int f = 0; f < static_cast<int>(g.tris.size()); ++f) {
        if (faceArea(g.faceEdgeIds, g.orients, g.edgeDiff, f) < 0) {
            ++n;
        }
    }
    return n;
}

// --- Phase 5d: self-contained bounded ternary-CSP solver -----------------------
// The flip-repair SAT patch (QuadriFlow FixFlipSat) is a small constraint problem
// over ternary variables v[i] in {-1,0,+1} (the unit edge-diff components): hard
// linear equalities (per-face loop closure) and hard inequalities (per-face
// no-flip, signed area >= 0). QuadriFlow solves it by shelling out to the external
// `minisat` binary (localsat.cpp:39); we forbid that dependency, so this is a
// self-contained backtracking search — arc-consistency on the equalities, most-
// constrained-variable branching preferring each variable's current value (minimal
// change, like minisat's polarity bias), bounded by a node budget so it stays
// local and never hangs. On Sat it writes the assignment back; on Unsat/Timeout it
// leaves `value` untouched (identical no-op semantics to minisat UNSAT / timeout).

enum class SatStatus { Sat, Unsat, Timeout };

struct EqRow {  // sa*v[a] + sb*v[b] + sc*v[c] == 0
    int a, b, c, sa, sb, sc;
};
struct GeRow {  // k0*v[a]*v[b] - k1*v[c]*v[d] >= 0  (a face's signed area)
    int a, b, c, d, k0, k1;
};

class TernaryCsp {
public:
    TernaryCsp(std::vector<int>& value, const std::vector<char>& flexible, std::vector<EqRow> eqs,
               std::vector<GeRow> ges, long nodeCap)
        : m_value(value), m_eqs(std::move(eqs)), m_ges(std::move(ges)), m_cap(nodeCap) {
        m_dom.resize(value.size());
        for (std::size_t i = 0; i < value.size(); ++i) {
            m_dom[i] = flexible[i] ? std::uint8_t{0b111} : maskOf(value[i]);
        }
    }

    SatStatus solve() {
        if (!propagate()) {
            return SatStatus::Unsat;
        }
        const SatStatus s = search();
        if (s == SatStatus::Sat) {
            for (std::size_t i = 0; i < m_dom.size(); ++i) {
                m_value[i] = lowValue(m_dom[i]);
            }
        }
        return s;
    }

private:
    static std::uint8_t maskOf(int v) { return static_cast<std::uint8_t>(1 << (v + 1)); }
    static bool has(std::uint8_t d, int v) { return (d & maskOf(v)) != 0; }
    static int popcnt(std::uint8_t d) { return (d & 1) + ((d >> 1) & 1) + ((d >> 2) & 1); }
    static int lowValue(std::uint8_t d) { return (d & 1) ? -1 : ((d & 2) ? 0 : 1); }

    // Generalised arc-consistency over one equality's three ternary variables:
    // keep only values that appear in some in-domain zero-sum tuple.
    bool narrowEq(const EqRow& e, bool& changed) {
        std::uint8_t okA = 0, okB = 0, okC = 0;
        for (int va = -1; va <= 1; ++va) {
            if (!has(m_dom[static_cast<std::size_t>(e.a)], va)) continue;
            for (int vb = -1; vb <= 1; ++vb) {
                if (!has(m_dom[static_cast<std::size_t>(e.b)], vb)) continue;
                for (int vc = -1; vc <= 1; ++vc) {
                    if (!has(m_dom[static_cast<std::size_t>(e.c)], vc)) continue;
                    if (e.sa * va + e.sb * vb + e.sc * vc == 0) {
                        okA |= maskOf(va);
                        okB |= maskOf(vb);
                        okC |= maskOf(vc);
                    }
                }
            }
        }
        return narrow(e.a, okA, changed) && narrow(e.b, okB, changed) &&
               narrow(e.c, okC, changed);
    }

    // A no-flip inequality: enforce only once <= 1 of its 4 vars is still free.
    bool checkGe(const GeRow& g, bool& changed) {
        const int idx[4] = {g.a, g.b, g.c, g.d};
        int freeVar = -1, nFree = 0;
        for (const int v : idx) {
            if (popcnt(m_dom[static_cast<std::size_t>(v)]) > 1) {
                freeVar = v;
                ++nFree;
            }
        }
        const auto cur = [&](int i) { return lowValue(m_dom[static_cast<std::size_t>(i)]); };
        const auto pick = [&](int i, int trial) { return i == freeVar ? trial : cur(i); };
        const auto area = [&](int t) {
            return g.k0 * pick(g.a, t) * pick(g.b, t) - g.k1 * pick(g.c, t) * pick(g.d, t);
        };
        if (nFree == 0) {
            return g.k0 * cur(g.a) * cur(g.b) - g.k1 * cur(g.c) * cur(g.d) >= 0;
        }
        if (nFree == 1) {
            std::uint8_t ok = 0;
            for (int val = -1; val <= 1; ++val) {
                if (has(m_dom[static_cast<std::size_t>(freeVar)], val) && area(val) >= 0) {
                    ok |= maskOf(val);
                }
            }
            return narrow(freeVar, ok, changed);
        }
        return true;
    }

    bool narrow(int var, std::uint8_t ok, bool& changed) {
        const std::size_t uv = static_cast<std::size_t>(var);
        const std::uint8_t nd = static_cast<std::uint8_t>(m_dom[uv] & ok);
        if (nd != m_dom[uv]) {
            m_dom[uv] = nd;
            changed = true;
        }
        return nd != 0;
    }

    bool propagate() {
        bool changed = true;
        while (changed) {
            changed = false;
            for (const EqRow& e : m_eqs) {
                if (!narrowEq(e, changed)) return false;
            }
            for (const GeRow& g : m_ges) {
                if (!checkGe(g, changed)) return false;
            }
        }
        return true;
    }

    int pickVar() const {
        int best = -1, bestPc = 4;
        for (std::size_t i = 0; i < m_dom.size(); ++i) {
            const int pc = popcnt(m_dom[i]);
            if (pc > 1 && pc < bestPc) {
                bestPc = pc;
                best = static_cast<int>(i);
            }
        }
        return best;
    }

    SatStatus search() {
        if (++m_nodes > m_cap) {
            return SatStatus::Timeout;
        }
        const int v = pickVar();
        if (v == -1) {
            return SatStatus::Sat;  // every domain singleton and consistent
        }
        const std::size_t uv = static_cast<std::size_t>(v);
        // Value order: the variable's current value first (minimal-change bias),
        // then the remaining two — always covers all of {-1,0,+1}.
        const int cur = m_value[uv];
        int order[3];
        int k = 0;
        if (cur >= -1 && cur <= 1) {
            order[k++] = cur;
        }
        for (int val = -1; val <= 1; ++val) {
            if (val != cur) {
                order[k++] = val;
            }
        }
        for (const int val : order) {
            if (!has(m_dom[uv], val)) continue;
            const std::vector<std::uint8_t> saved = m_dom;
            m_dom[uv] = maskOf(val);
            if (propagate()) {
                const SatStatus s = search();
                if (s != SatStatus::Unsat) {
                    return s;  // Sat (keep domains) or Timeout (abort)
                }
            }
            m_dom = saved;
        }
        return SatStatus::Unsat;
    }

    std::vector<int>& m_value;
    std::vector<std::uint8_t> m_dom;
    std::vector<EqRow> m_eqs;
    std::vector<GeRow> m_ges;
    long m_nodes = 0;
    long m_cap;
};

SatStatus solveTernaryCsp(std::vector<int>& value, const std::vector<char>& flexible,
                          std::vector<EqRow> eqs, std::vector<GeRow> ges, long nodeCap) {
    TernaryCsp csp(value, flexible, std::move(eqs), std::move(ges), nodeCap);
    return csp.solve();
}

// A coarsened edge-graph hierarchy over (FQ, F2E, E2F, EdgeDiff): each level
// contracts an independent set of zero-diff edges, chaining orientations across
// merged strips. `back()` is the coarsest grid where each face is ~one integer cell.
struct EdgeHierarchy {
    std::vector<std::vector<std::array<int, 3>>> FQ, F2E;
    std::vector<std::vector<Int2Pair>> E2F;
    std::vector<std::vector<Int2>> edgeDiff;
    std::vector<std::vector<int>> allow, sing;
    std::vector<std::vector<int>> toUpperEdges, toUpperOrients, toUpperFaces;
    int levels = 0;
    bool consistent = true;

    void downsample(std::vector<std::array<int, 3>> fq, std::vector<std::array<int, 3>> f2e,
                    std::vector<Int2> ediff, std::vector<int> allowChanges, int level);
    void fixFlip(int depth = 0);
    int fixFlipSat(int level, int threshold);
    void propagateEdge();
    void updateGraphValue(std::vector<std::array<int, 3>>& fq, std::vector<std::array<int, 3>>& f2e,
                          std::vector<Int2>& ediff);
};

void EdgeHierarchy::downsample(std::vector<std::array<int, 3>> fq,
                               std::vector<std::array<int, 3>> f2e, std::vector<Int2> ediff,
                               std::vector<int> allowChanges, int level) {
    const auto uz = [](int x) { return static_cast<std::size_t>(x); };
    const auto isZero = [](Int2 d) { return d.x == 0 && d.y == 0; };

    std::vector<Int2Pair> e2f(ediff.size(), Int2Pair{-1, -1});
    for (int i = 0; i < static_cast<int>(f2e.size()); ++i) {
        for (int j = 0; j < 3; ++j) {
            const int e = f2e[uz(i)][uz(j)];
            if (e2f[uz(e)][0] == -1) {
                e2f[uz(e)][0] = i;
            } else {
                e2f[uz(e)][1] = i;
            }
        }
    }
    const int lv = (level == -1) ? 100 : level;
    FQ.resize(uz(lv));
    F2E.resize(uz(lv));
    E2F.resize(uz(lv));
    edgeDiff.resize(uz(lv));
    allow.resize(uz(lv));
    sing.resize(uz(lv));
    toUpperEdges.resize(uz(lv - 1));
    toUpperOrients.resize(uz(lv - 1));

    for (int i = 0; i < static_cast<int>(fq.size()); ++i) {
        Int2 d{0, 0};
        for (int j = 0; j < 3; ++j) {
            const Int2 c = rshift90i(ediff[uz(f2e[uz(i)][uz(j)])], fq[uz(i)][uz(j)]);
            d.x += c.x;
            d.y += c.y;
        }
        if (!isZero(d)) {
            sing[0].push_back(i);
        }
    }
    allow[0] = std::move(allowChanges);
    FQ[0] = std::move(fq);
    F2E[0] = std::move(f2e);
    E2F[0] = std::move(e2f);
    edgeDiff[0] = std::move(ediff);

    levels = lv;
    for (int l = 0; l < lv - 1; ++l) {
        auto& cFQ = FQ[uz(l)];
        auto& cE2F = E2F[uz(l)];
        auto& cF2E = F2E[uz(l)];
        auto& cAllow = allow[uz(l)];
        auto& cDiff = edgeDiff[uz(l)];
        auto& cSing = sing[uz(l)];
        std::vector<int> fixedFaces(cF2E.size(), 0);
        for (const int s : cSing) {
            fixedFaces[uz(s)] = 1;
        }
        auto& toUp = toUpperEdges[uz(l)];
        auto& toUpO = toUpperOrients[uz(l)];
        toUp.assign(cE2F.size(), -1);
        toUpO.assign(cE2F.size(), 0);
        auto& nFQ = FQ[uz(l + 1)];
        auto& nE2F = E2F[uz(l + 1)];
        auto& nF2E = F2E[uz(l + 1)];
        auto& nAllow = allow[uz(l + 1)];
        auto& nDiff = edgeDiff[uz(l + 1)];
        auto& nSing = sing[uz(l + 1)];

        // Select an independent set of zero-diff edges to contract (freeze the
        // 1-ring of each committed edge so neighbours never contract together).
        for (int i = 0; i < static_cast<int>(cE2F.size()); ++i) {
            if (!isZero(cDiff[uz(i)])) {
                continue;
            }
            if ((cE2F[uz(i)][0] >= 0 && fixedFaces[uz(cE2F[uz(i)][0])]) ||
                (cE2F[uz(i)][1] >= 0 && fixedFaces[uz(cE2F[uz(i)][1])])) {
                continue;
            }
            for (int j = 0; j < 2; ++j) {
                const int f = cE2F[uz(i)][uz(j)];
                if (f < 0) {
                    continue;
                }
                for (int k = 0; k < 3; ++k) {
                    const int ne = cF2E[uz(f)][uz(k)];
                    for (int m = 0; m < 2; ++m) {
                        const int nf = cE2F[uz(ne)][uz(m)];
                        if (nf >= 0 && fixedFaces[uz(nf)] == 0) {
                            fixedFaces[uz(nf)] = 1;
                        }
                    }
                }
            }
            if (cE2F[uz(i)][0] >= 0) {
                fixedFaces[uz(cE2F[uz(i)][0])] = 2;
            }
            if (cE2F[uz(i)][1] >= 0) {
                fixedFaces[uz(cE2F[uz(i)][1])] = 2;
            }
            toUp[uz(i)] = -2;
        }
        for (int i = 0; i < static_cast<int>(cE2F.size()); ++i) {
            if (toUp[uz(i)] == -2) {
                continue;
            }
            const bool a = cE2F[uz(i)][0] < 0 || fixedFaces[uz(cE2F[uz(i)][0])] == 2;
            const bool b = cE2F[uz(i)][1] < 0 || fixedFaces[uz(cE2F[uz(i)][1])] == 2;
            if (a && b) {
                toUp[uz(i)] = -3;
            }
        }

        // Emit coarse edges: survivors keep their diff; edges bordering a consumed
        // face walk a merge path across consumed faces, accumulating orientation.
        int numE = 0;
        for (int i = 0; i < static_cast<int>(toUp.size()); ++i) {
            if (toUp[uz(i)] != -1) {
                continue;
            }
            const bool sa = cE2F[uz(i)][0] < 0 || fixedFaces[uz(cE2F[uz(i)][0])] < 2;
            const bool sb = cE2F[uz(i)][1] < 0 || fixedFaces[uz(cE2F[uz(i)][1])] < 2;
            if (sa && sb) {
                nE2F.push_back(cE2F[uz(i)]);
                toUpO[uz(i)] = 0;
                toUp[uz(i)] = numE++;
                continue;
            }
            const bool useSlot0 = cE2F[uz(i)][1] < 0 || fixedFaces[uz(cE2F[uz(i)][0])] < 2;
            const int f0 = useSlot0 ? cE2F[uz(i)][0] : cE2F[uz(i)][1];
            int e = i, f = f0;
            std::vector<std::pair<int, int>> paths;
            paths.emplace_back(i, 0);
            while (true) {
                if (cE2F[uz(e)][0] == f) {
                    f = cE2F[uz(e)][1];
                } else if (cE2F[uz(e)][1] == f) {
                    f = cE2F[uz(e)][0];
                }
                if (f < 0 || fixedFaces[uz(f)] < 2) {
                    for (int j = 0; j < static_cast<int>(paths.size()); ++j) {
                        toUp[uz(paths[uz(j)].first)] = numE;
                        int orient = paths[uz(j)].second;
                        if (j > 0) {
                            orient = (orient + toUpO[uz(paths[uz(j - 1)].first)]) % 4;
                        }
                        toUpO[uz(paths[uz(j)].first)] = orient;
                    }
                    nE2F.push_back(Int2Pair{f0, f});
                    ++numE;
                    break;
                }
                int ind0 = -1, ind1 = -1;
                for (int j = 0; j < 3; ++j) {
                    if (cF2E[uz(f)][uz(j)] == e) {
                        ind0 = j;
                        break;
                    }
                }
                for (int j = 0; j < 3; ++j) {
                    const int e1 = cF2E[uz(f)][uz(j)];
                    if (e1 != e && toUp[uz(e1)] != -2) {
                        e = e1;
                        ind1 = j;
                        break;
                    }
                }
                if (ind1 != -1) {
                    paths.emplace_back(e, (cFQ[uz(f)][uz(ind1)] - cFQ[uz(f)][uz(ind0)] + 6) % 4);
                } else {
                    if (!isZero(cDiff[uz(e)])) {
                        consistent = false;  // "Unsatisfied": grid violates zero-sum
                    }
                    for (const auto& p : paths) {
                        toUp[uz(p.first)] = numE;
                        toUpO[uz(p.first)] = 0;
                    }
                    ++numE;
                    nE2F.push_back(Int2Pair{f0, f0});
                    break;
                }
            }
        }

        nDiff.assign(uz(numE), Int2{0, 0});
        nAllow.assign(uz(numE * 2), 1);
        for (int i = 0; i < static_cast<int>(toUp.size()); ++i) {
            if (toUp[uz(i)] < 0) {
                continue;
            }
            if (toUpO[uz(i)] == 0) {
                nDiff[uz(toUp[uz(i)])] = cDiff[uz(i)];
            }
            const int dim = toUpO[uz(i)] % 2;
            if (cAllow[uz(i * 2 + dim)] == 0) {
                nAllow[uz(toUp[uz(i)] * 2)] = 0;
            } else if (cAllow[uz(i * 2 + dim)] == 2) {
                nAllow[uz(toUp[uz(i)] * 2)] = 2;
            }
            if (cAllow[uz(i * 2 + 1 - dim)] == 0) {
                nAllow[uz(toUp[uz(i)] * 2 + 1)] = 0;
            } else if (cAllow[uz(i * 2 + 1 - dim)] == 2) {
                nAllow[uz(toUp[uz(i)] * 2 + 1)] = 2;
            }
        }

        std::vector<int> upperFace(cF2E.size(), -1);
        for (int i = 0; i < static_cast<int>(cF2E.size()); ++i) {
            const std::array<int, 3> eid{toUp[uz(cF2E[uz(i)][0])], toUp[uz(cF2E[uz(i)][1])],
                                         toUp[uz(cF2E[uz(i)][2])]};
            if (eid[0] >= 0 && eid[1] >= 0 && eid[2] >= 0) {
                std::array<int, 3> eido{};
                for (int j = 0; j < 3; ++j) {
                    eido[uz(j)] = (cFQ[uz(i)][uz(j)] + 4 - toUpO[uz(cF2E[uz(i)][uz(j)])]) % 4;
                }
                upperFace[uz(i)] = static_cast<int>(nF2E.size());
                nF2E.push_back(eid);
                nFQ.push_back(eido);
            }
        }
        for (auto& p : nE2F) {
            for (int j = 0; j < 2; ++j) {
                if (p[uz(j)] >= 0) {
                    p[uz(j)] = upperFace[uz(p[uz(j)])];
                }
            }
        }
        for (const int s : cSing) {
            if (upperFace[uz(s)] >= 0) {
                nSing.push_back(upperFace[uz(s)]);
            }
        }
        toUpperFaces.push_back(std::move(upperFace));

        if (nDiff.size() == cDiff.size()) {  // fixpoint: no edge contracted
            levels = l + 1;
            break;
        }
    }
    FQ.resize(uz(levels));
    F2E.resize(uz(levels));
    E2F.resize(uz(levels));
    edgeDiff.resize(uz(levels));
    allow.resize(uz(levels));
    sing.resize(uz(levels));
    toUpperEdges.resize(uz(levels - 1));
    toUpperOrients.resize(uz(levels - 1));
}

// Repairs flipped (negative-area) cells on the coarsest level, then propagates the
// repair down to level 0 (QuadriFlow FixFlip + CheckShrink, hierarchy.cpp:923). A
// checkShrink shifts a vertex-ring's edge diffs and commits only when it strictly
// reduces the flip count — the monotone-decrease that guarantees termination.
void EdgeHierarchy::fixFlip(int depth) {
    const auto uz = [](int x) { return static_cast<std::size_t>(x); };
    const int l = levels - 1;
    auto& cF2E = F2E[uz(l)];
    auto& cE2F = E2F[uz(l)];
    auto& cFQ = FQ[uz(l)];
    auto& cDiff = edgeDiff[uz(l)];
    auto& cAllow = allow[uz(l)];

    // Directed half-edge opposite for this level (QuadriFlow FixFlip E2E, :932):
    // t1 scans forward in the first incident face, t2 backward in the second. This
    // is NOT the mesh half-edge opposite; the ring walks below depend on it.
    std::vector<int> e2e(cF2E.size() * 3, -1);
    for (int i = 0; i < static_cast<int>(cE2F.size()); ++i) {
        const int v1 = cE2F[uz(i)][0], v2 = cE2F[uz(i)][1];
        int t1 = 0, t2 = 2;
        if (v1 != -1) {
            while (cF2E[uz(v1)][uz(t1)] != i) {
                ++t1;
            }
        }
        if (v2 != -1) {
            while (cF2E[uz(v2)][uz(t2)] != i) {
                --t2;
            }
        }
        if (v1 != -1) {
            e2e[uz(v1 * 3 + t1)] = (v2 == -1) ? -1 : v2 * 3 + t2;
        }
        if (v2 != -1) {
            e2e[uz(v2 * 3 + t2)] = (v1 == -1) ? -1 : v1 * 3 + t1;
        }
    }

    const auto area = [&](int f) { return faceArea(cF2E, cFQ, cDiff, f); };

    // Try to shrink the vertex ring reached from directed edge `deid` so every cell
    // there fits `allowedLen`; commit iff it lowers the ring's flip count.
    const auto checkShrink = [&](int deid, int allowedLen) -> bool {
        if (deid == -1) {
            return false;
        }
        std::vector<int> corrFaces, corrEdges;
        std::vector<Int2> corrDiff;
        const int deid0 = deid;
        while (deid != -1) {
            deid = deid / 3 * 3 + (deid + 2) % 3;
            if (e2e[uz(deid)] == -1) {
                break;
            }
            deid = e2e[uz(deid)];
            if (deid == deid0) {
                break;
            }
        }
        Int2 diff = cDiff[uz(cF2E[uz(deid / 3)][uz(deid % 3)])];
        do {
            corrDiff.push_back(diff);
            corrEdges.push_back(deid);
            corrFaces.push_back(deid / 3);
            deid = e2e[uz(deid)];
            if (deid == -1) {
                return false;
            }
            const Int2 r = rshift90i(diff, cFQ[uz(deid / 3)][uz(deid % 3)]);
            diff = Int2{-r.x, -r.y};
            deid = deid / 3 * 3 + (deid + 1) % 3;
            diff = rshift90i(diff, (4 - cFQ[uz(deid / 3)][uz(deid % 3)]) % 4);
        } while (deid != corrEdges.front());
        if (diff.x != corrDiff.front().x || diff.y != corrDiff.front().y) {
            return false;
        }
        std::unordered_map<int, Int2> newValues;
        for (const int de : corrEdges) {
            const int eid = cF2E[uz(de / 3)][uz(de % 3)];
            newValues[eid] = cDiff[uz(eid)];
        }
        for (std::size_t i = 0; i < corrEdges.size(); ++i) {
            const int eid = cF2E[uz(corrEdges[i] / 3)][uz(corrEdges[i] % 3)];
            if ((corrDiff[i].x != 0 && cAllow[uz(eid * 2)] == 0) ||
                (corrDiff[i].y != 0 && cAllow[uz(eid * 2 + 1)] == 0)) {
                return false;
            }
            Int2& res = newValues[eid];
            res.x -= corrDiff[i].x;
            res.y -= corrDiff[i].y;
            if (std::abs(res.x) > allowedLen || std::abs(res.y) > allowedLen) {
                return false;
            }
            if ((std::abs(res.x) > 1 && res.y != 0) || (std::abs(res.y) > 1 && res.x != 0)) {
                return false;
            }
        }
        int prevFlips = 0, curFlips = 0;
        for (const int f : corrFaces) {
            if (area(f) < 0) {
                ++prevFlips;
            }
        }
        for (auto& p : newValues) {
            std::swap(cDiff[uz(p.first)], p.second);
        }
        for (const int f : corrFaces) {
            if (area(f) < 0) {
                ++curFlips;
            }
        }
        if (curFlips < prevFlips) {
            return true;
        }
        for (auto& p : newValues) {  // no improvement — revert
            std::swap(cDiff[uz(p.first)], p.second);
        }
        return false;
    };

    std::queue<int> flipped;
    for (int i = 0; i < static_cast<int>(cF2E.size()); ++i) {
        if (area(i) < 0) {
            flipped.push(i);
        }
    }
    bool update = false;
    for (int maxLen = 1; !update && maxLen <= 2; ++maxLen) {
        while (!flipped.empty()) {
            const int f = flipped.front();
            if (area(f) >= 0) {
                flipped.pop();
                continue;
            }
            for (int i = 0; i < 3; ++i) {
                if (checkShrink(f * 3 + i, maxLen) || checkShrink(e2e[uz(f * 3 + i)], maxLen)) {
                    update = true;
                    break;
                }
            }
            flipped.pop();
        }
    }
    // Re-coarsen the repaired level and repair again (QuadriFlow recurses): each
    // pass strictly lowers the flip count, so this terminates; depth-capped anyway.
    if (update && depth < 64) {
        EdgeHierarchy fh;
        fh.downsample(FQ[uz(l)], F2E[uz(l)], edgeDiff[uz(l)], allow[uz(l)], -1);
        fh.fixFlip(depth + 1);
        fh.updateGraphValue(FQ[uz(l)], F2E[uz(l)], edgeDiff[uz(l)]);
    }
    propagateEdge();
}

// SAT-based flip repair (QuadriFlow FixFlipSat, hierarchy.cpp:640): extract a local
// patch of flexible edges around the flipped cells (BFS bounded by `threshold`),
// split it into connected groups, and for each group build + solve the ternary CSP
// (per-face loop-closure equalities + no-flip area inequalities) with our
// self-contained solver — clearing the multi-cell clusters greedy CheckShrink
// cannot. Mutates only edgeDiff on `level`; returns the post-repair flip count.
int EdgeHierarchy::fixFlipSat(int level, int threshold) {
    const auto uz = [](int x) { return static_cast<std::size_t>(x); };
    auto& cF2E = F2E[uz(level)];
    auto& cE2F = E2F[uz(level)];
    auto& cFQ = FQ[uz(level)];
    auto& cDiff = edgeDiff[uz(level)];
    auto& cAllow = allow[uz(level)];

    std::vector<int> e2e(cF2E.size() * 3, -1);
    for (int i = 0; i < static_cast<int>(cE2F.size()); ++i) {
        const int v1 = cE2F[uz(i)][0], v2 = cE2F[uz(i)][1];
        int t1 = 0, t2 = 2;
        if (v1 != -1) {
            while (cF2E[uz(v1)][uz(t1)] != i) ++t1;
        }
        if (v2 != -1) {
            while (cF2E[uz(v2)][uz(t2)] != i) --t2;
        }
        if (v1 != -1) {
            e2e[uz(v1 * 3 + t1)] = (v2 == -1) ? -1 : v2 * 3 + t2;
        }
        if (v2 != -1) {
            e2e[uz(v2 * 3 + t2)] = (v1 == -1) ? -1 : v1 * 3 + t1;
        }
    }
    const auto area = [&](int f) { return faceArea(cF2E, cFQ, cDiff, f); };
    const auto elen = [&](int he) {
        const Int2 d = cDiff[uz(cF2E[uz(he / 3)][uz(he % 3)])];
        return std::abs(d.x) + std::abs(d.y);
    };

    // Patch BFS: mark dedges reachable from flipped faces (free flood across
    // zero-length edges; length-weighted expansion up to `threshold`).
    std::deque<std::pair<int, int>> queue;
    std::vector<char> mark(cF2E.size() * 3, 0);
    for (int f = 0; f < static_cast<int>(cF2E.size()); ++f) {
        if (area(f) < 0) {
            for (int j = 0; j < 3; ++j) {
                if (!mark[uz(f * 3 + j)]) {
                    queue.emplace_back(f * 3 + j, 0);
                    mark[uz(f * 3 + j)] = 1;
                }
            }
        }
    }
    const auto sweep = [&](int e0, int d, bool zeroLen, int step) {
        int e = e0, e1 = -1;
        do {
            e1 = e2e[uz(e)];
            if (e1 == -1) break;
            const int len = elen(e1);
            const bool take = zeroLen ? (len == 0) : (len > 0 && d + len <= threshold);
            if (take && !mark[uz(e1)]) {
                mark[uz(e1)] = 1;
                if (zeroLen) {
                    queue.emplace_front(e1, d);
                } else {
                    queue.emplace_back(e1, d + len);
                }
            }
            e = e1 / 3 * 3 + (e1 + step) % 3;
            mark[uz(e)] = 1;
        } while (e != e0);
        return e1;
    };
    while (!queue.empty()) {
        const int e0 = queue.front().first, d = queue.front().second;
        queue.pop_front();
        if (sweep(e0, d, true, 1) == -1) sweep(e0, d, true, 2);
        if (sweep(e0, d, false, 1) == -1) sweep(e0, d, false, 2);
    }

    std::vector<char> flexible(cDiff.size(), 0);
    for (int i = 0; i < static_cast<int>(cF2E.size()); ++i) {
        for (int j = 0; j < 3; ++j) {
            if (mark[uz(i * 3 + j)]) {
                flexible[uz(cF2E[uz(i)][uz(j)])] = 1;
            }
        }
    }
    for (int i = 0; i < static_cast<int>(cDiff.size()); ++i) {
        if (cE2F[uz(i)][0] == cE2F[uz(i)][1] || cAllow[uz(i * 2)] == 0 || cAllow[uz(i * 2 + 1)] == 0) {
            flexible[uz(i)] = 0;
        }
    }

    // Group flexible edges into connected components (via shared faces).
    int numGroup = 0;
    std::vector<int> group(cDiff.size(), -1), index(cDiff.size(), -1);
    for (int i = 0; i < static_cast<int>(cDiff.size()); ++i) {
        if (group[uz(i)] != -1 || !flexible[uz(i)]) {
            continue;
        }
        std::queue<int> bfs;
        bfs.push(i);
        group[uz(i)] = numGroup;
        while (!bfs.empty()) {
            const int e = bfs.front();
            bfs.pop();
            for (int s = 0; s < 2; ++s) {
                const int f = cE2F[uz(e)][uz(s)];
                if (f == -1) {
                    continue;
                }
                for (int k = 0; k < 3; ++k) {
                    const int e1 = cF2E[uz(f)][uz(k)];
                    if (flexible[uz(e1)] && group[uz(e1)] == -1) {
                        group[uz(e1)] = numGroup;
                        bfs.push(e1);
                    }
                }
            }
        }
        ++numGroup;
    }

    // Per-group: flat ternary variables (edge diff components) + face constraints.
    std::vector<int> numEdges(static_cast<std::size_t>(numGroup), 0);
    std::vector<std::vector<int>> values(static_cast<std::size_t>(numGroup));
    for (int i = 0; i < static_cast<int>(cDiff.size()); ++i) {
        if (group[uz(i)] != -1) {
            const std::size_t grp = uz(group[uz(i)]);
            index[uz(i)] = numEdges[grp]++;
            values[grp].push_back(cDiff[uz(i)].x);
            values[grp].push_back(cDiff[uz(i)].y);
        }
    }
    const std::vector<int> numFlex = numEdges;  // flexible count, before fixed appends
    std::vector<std::vector<EqRow>> eqs(static_cast<std::size_t>(numGroup));
    std::vector<std::vector<GeRow>> ges(static_cast<std::size_t>(numGroup));
    std::map<std::pair<int, int>, int> fixedVars;
    const auto sgn = [](int v) { return (v > 0) - (v < 0); };
    for (int i = 0; i < static_cast<int>(cF2E.size()); ++i) {
        int gind = 0;
        while (gind < 3 && group[uz(cF2E[uz(i)][uz(gind)])] == -1) {
            ++gind;
        }
        if (gind == 3) {
            continue;
        }
        const int grp = group[uz(cF2E[uz(i)][uz(gind)])];
        int ind[3] = {-1, -1, -1};
        for (int j = 0; j < 3; ++j) {
            const int eid = cF2E[uz(i)][uz(j)];
            if (group[uz(eid)] == grp) {
                ind[j] = index[uz(eid)];
            } else {  // fixed neighbour edge: append (or reuse) a pinned variable
                const std::pair<int, int> key{eid, grp};
                const auto it = fixedVars.find(key);
                if (it == fixedVars.end()) {
                    ind[j] = numEdges[uz(grp)];
                    values[uz(grp)].push_back(cDiff[uz(eid)].x);
                    values[uz(grp)].push_back(cDiff[uz(eid)].y);
                    fixedVars[key] = numEdges[uz(grp)]++;
                } else {
                    ind[j] = it->second;
                }
            }
        }
        Int2 var[3], cst[3];
        for (int j = 0; j < 3; ++j) {
            const Int2 v = rshift90i(Int2{ind[j] * 2 + 1, ind[j] * 2 + 2}, cFQ[uz(i)][uz(j)]);
            cst[j] = Int2{sgn(v.x), sgn(v.y)};
            var[j] = Int2{std::abs(v.x) - 1, std::abs(v.y) - 1};
        }
        eqs[uz(grp)].push_back(EqRow{var[0].x, var[1].x, var[2].x, cst[0].x, cst[1].x, cst[2].x});
        eqs[uz(grp)].push_back(EqRow{var[0].y, var[1].y, var[2].y, cst[0].y, cst[1].y, cst[2].y});
        ges[uz(grp)].push_back(GeRow{var[0].x, var[1].y, var[0].y, var[1].x,
                                     cst[0].x * cst[1].y, cst[0].y * cst[1].x});
    }

    for (int gi = 0; gi < numGroup; ++gi) {
        std::vector<char> flex(values[uz(gi)].size(), 1);
        for (std::size_t j = uz(numFlex[uz(gi)] * 2); j < flex.size(); ++j) {
            flex[j] = 0;
        }
        solveTernaryCsp(values[uz(gi)], flex, eqs[uz(gi)], ges[uz(gi)], 50000);
    }
    for (int i = 0; i < static_cast<int>(cDiff.size()); ++i) {
        const int grp = group[uz(i)];
        if (grp == -1) {
            continue;
        }
        cDiff[uz(i)].x = values[uz(grp)][uz(2 * index[uz(i)])];
        cDiff[uz(i)].y = values[uz(grp)][uz(2 * index[uz(i)] + 1)];
    }
    int flips = 0;
    for (int f = 0; f < static_cast<int>(cF2E.size()); ++f) {
        if (area(f) < 0) {
            ++flips;
        }
    }
    return flips;
}

void EdgeHierarchy::propagateEdge() {
    const auto uz = [](int x) { return static_cast<std::size_t>(x); };
    for (int level = static_cast<int>(toUpperEdges.size()); level > 0; --level) {
        const auto& cDiff = edgeDiff[uz(level)];
        auto& nDiff = edgeDiff[uz(level - 1)];
        const auto& cFQ = FQ[uz(level)];
        auto& nFQ = FQ[uz(level - 1)];
        const auto& nF2E = F2E[uz(level - 1)];
        const auto& toUp = toUpperEdges[uz(level - 1)];
        const auto& toUpFace = toUpperFaces[uz(level - 1)];
        const auto& toUpO = toUpperOrients[uz(level - 1)];
        for (int i = 0; i < static_cast<int>(toUp.size()); ++i) {
            nDiff[uz(i)] = toUp[uz(i)] >= 0
                               ? rshift90i(cDiff[uz(toUp[uz(i)])], (4 - toUpO[uz(i)]) % 4)
                               : Int2{0, 0};
        }
        for (int i = 0; i < static_cast<int>(toUpFace.size()); ++i) {
            if (toUpFace[uz(i)] == -1) {
                continue;
            }
            const auto& eido = cFQ[uz(toUpFace[uz(i)])];
            for (int j = 0; j < 3; ++j) {
                nFQ[uz(i)][uz(j)] = (eido[uz(j)] + toUpO[uz(nF2E[uz(i)][uz(j)])]) % 4;
            }
        }
    }
}

void EdgeHierarchy::updateGraphValue(std::vector<std::array<int, 3>>& fq,
                                     std::vector<std::array<int, 3>>& f2e,
                                     std::vector<Int2>& ediff) {
    fq = std::move(FQ[0]);
    f2e = std::move(F2E[0]);
    ediff = std::move(edgeDiff[0]);
}

// Repairs the integer grid's flipped/quantized cells in place (Milestone 5a wires
// the coarsen + propagate round-trip; 5b adds the fixFlip repair). Only edgeDiff
// changes; faceEdgeIds / orients round-trip identically.
void fixFlipHierarchy(IntegerGrid& g) {
    std::vector<int> allowChanges(g.edgeDiff.size() * 2, 1);
    EdgeHierarchy h;
    h.downsample(g.orients, g.faceEdgeIds, g.edgeDiff, allowChanges, -1);
    if (!h.consistent) {
        return;  // invalid grid (non-integrable) — leave edge_diff untouched
    }
    h.fixFlip();  // repairs flipped cells on the coarsest level + propagates down
    h.updateGraphValue(g.orients, g.faceEdgeIds, g.edgeDiff);
}

// SAT-based flip repair over the coarsened grid, escalating the patch threshold
// 1..4 (QuadriFlow FixFlipSat driver, parametrizer-flip.cpp:175). Requires UNIT
// edge diffs (ternary domain), so it runs on the post-subdivide grid. Coarsest-
// only: propagateEdge floods the repair to level 0, then updateGraphValue reads it.
void fixFlipSatHierarchy(IntegerGrid& g) {
    for (int threshold = 1; threshold <= 4; ++threshold) {
        std::vector<int> allowChanges(g.edgeDiff.size() * 2, 1);
        EdgeHierarchy h;
        h.downsample(g.orients, g.faceEdgeIds, g.edgeDiff, allowChanges, -1);
        if (!h.consistent) {
            return;
        }
        const int nflip = h.fixFlipSat(h.levels - 1, threshold);
        h.propagateEdge();
        h.updateGraphValue(g.orients, g.faceEdgeIds, g.edgeDiff);
        if (nflip == 0) {
            break;
        }
    }
}

}  // namespace

IntegerSolveStats debugIntegerSolve(const Mesh& mesh, const PositionField& field) {
    const IntegerGrid g = computeIntegerGrid(mesh, field);
    IntegerSolveStats st;
    st.faces = g.tris.size();
    st.preSingular = g.singular.size();
    st.residualMismatch = g.residualMismatch;
    st.flow = g.flow;
    st.supply = g.supply;
    st.postSingular = countResidualSingularities(g);
    return st;
}

namespace {

// Small integer-2D vector arithmetic (component-wise; i2half truncates toward
// zero, matching Eigen Vector2i / 2).
Int2 i2add(Int2 a, Int2 b) { return {a.x + b.x, a.y + b.y}; }
Int2 i2sub(Int2 a, Int2 b) { return {a.x - b.x, a.y - b.y}; }
Int2 i2neg(Int2 a) { return {-a.x, -a.y}; }
Int2 i2half(Int2 a) { return {a.x / 2, a.y / 2}; }
bool i2eq(Int2 a, Int2 b) { return a.x == b.x && a.y == b.y; }
int i2maxComp(Int2 a) { return std::max(std::abs(a.x), std::abs(a.y)); }

// The subdivided triangle mesh: per-half-edge local-frame integer diffs, the
// half-edge opposite map, and per-vertex field (O) + mesh (P) positions. After
// subdivideToUnitCells every diff has |component| <= maxLen.
struct SubMesh {
    std::vector<std::array<int, 3>> tris;
    std::vector<Int2> diffs;  // 3 * |tris|, half-edge (t*3+j) = edge tris[t][j]->tris[t][j+1]
    std::vector<int> e2e;     // 3 * |tris|, opposite half-edge (-1 on boundary)
    std::vector<Vec3> o;      // field lattice position, per vertex (output location)
    std::vector<Vec3> p;      // mesh position, per vertex (split-priority ordering)
    std::vector<Vec3> n;      // field normal, per vertex (for manifold hole filling)
};

// Splits every edge whose integer jump spans more than `maxLen` grid cells so each
// surviving edge spans <= maxLen. Faithful port of QuadriFlow subdivide_edgeDiff
// (subdivide.cpp) operating purely on per-half-edge local-frame diffs — the
// canonical edge_diff/face_edgeOrients mirror it keeps in lockstep is unneeded
// because the extractor's collapse (diff==0) and diagonal (|diff|==(1,1)) tests are
// rotation-invariant, and AnalyzeOrient/FixOrient never mutate diffs themselves.
SubMesh subdivideToUnitCells(const IntegerGrid& g, const Mesh& mesh, const PositionField& field,
                             int maxLen) {
    SubMesh m;
    const std::size_t vcap = mesh.vertexCapacity();
    m.o.resize(vcap);
    m.p.resize(vcap);
    m.n.resize(vcap);
    for (std::size_t v = 0; v < vcap; ++v) {
        if (field.valid[v]) {
            m.o[v] = field.o[v];
            m.p[v] = mesh.position(VertexId{static_cast<Index>(v)});
            m.n[v] = field.normal[v];
        }
    }
    m.tris.reserve(g.tris.size());
    for (const auto& t : g.tris) {
        m.tris.push_back({static_cast<int>(t[0]), static_cast<int>(t[1]), static_cast<int>(t[2])});
    }
    m.e2e = g.e2e;
    m.diffs.resize(g.tris.size() * 3);
    for (std::size_t t = 0; t < g.tris.size(); ++t) {
        for (std::size_t j = 0; j < 3; ++j) {
            m.diffs[t * 3 + j] = rshift90i(g.edgeDiff[static_cast<std::size_t>(g.faceEdgeIds[t][j])],
                                           g.orients[t][j]);
        }
    }

    const auto dedgeNext = [](int e) { return e / 3 * 3 + (e + 1) % 3; };
    const auto dedgePrev = [](int e) { return e / 3 * 3 + (e + 2) % 3; };
    const auto sqLen = [&](int a, int b) {
        return lengthSquared(m.p[static_cast<std::size_t>(a)] - m.p[static_cast<std::size_t>(b)]);
    };
    const auto vtx = [&](int he) { return m.tris[static_cast<std::size_t>(he / 3)][static_cast<std::size_t>(he % 3)]; };

    struct EdgeLink {
        int id;
        float length;
        int maxlen;
    };
    const auto cmp = [](const EdgeLink& a, const EdgeLink& b) { return a.maxlen < b.maxlen; };
    std::priority_queue<EdgeLink, std::vector<EdgeLink>, decltype(cmp)> queue(cmp);
    for (std::size_t i = 0; i < m.diffs.size(); ++i) {
        if (i2maxComp(m.diffs[i]) > maxLen) {
            const int e = static_cast<int>(i);
            const int other = m.e2e[i];
            if (other == -1 || other > e) {
                queue.push({e, sqLen(vtx(e), vtx(dedgeNext(e))), i2maxComp(m.diffs[i])});
            }
        }
    }

    const auto addFace = [&]() {
        const int f = static_cast<int>(m.tris.size());
        m.tris.push_back({0, 0, 0});
        m.diffs.insert(m.diffs.end(), {Int2{}, Int2{}, Int2{}});
        m.e2e.insert(m.e2e.end(), {-1, -1, -1});
        return f;
    };
    const auto sE2E = [&](int a, int b) {
        m.e2e[static_cast<std::size_t>(a)] = b;
        if (b != -1) {
            m.e2e[static_cast<std::size_t>(b)] = a;
        }
    };
    const auto schedule = [&](int f) {
        for (int i = 0; i < 3; ++i) {
            const Int2 d = m.diffs[static_cast<std::size_t>(f * 3 + i)];
            if (i2maxComp(d) > maxLen) {
                queue.push({f * 3 + i, sqLen(m.tris[static_cast<std::size_t>(f)][static_cast<std::size_t>(i)],
                                             m.tris[static_cast<std::size_t>(f)][static_cast<std::size_t>((i + 1) % 3)]),
                            i2maxComp(d)});
            }
        }
    };

    while (!queue.empty()) {
        const EdgeLink edge = queue.top();
        queue.pop();
        const int e0 = edge.id, e1 = m.e2e[static_cast<std::size_t>(e0)];
        const bool isBoundary = (e1 == -1);
        const int f0 = e0 / 3, f1 = isBoundary ? -1 : e1 / 3;
        const int v0 = vtx(e0), v0p = vtx(dedgePrev(e0)), v1 = vtx(dedgeNext(e0));
        if (sqLen(v0, v1) != edge.length) {
            continue;  // stale queue entry: this edge already changed
        }
        if (i2maxComp(m.diffs[static_cast<std::size_t>(e0)]) <= maxLen) {
            continue;
        }
        const int v1p = isBoundary ? -1 : vtx(dedgePrev(e1));

        // New split vertex at the edge midpoint (field + mesh position).
        const int vn = static_cast<int>(m.o.size());
        m.o.push_back((m.o[static_cast<std::size_t>(v0)] + m.o[static_cast<std::size_t>(v1)]) * 0.5f);
        m.p.push_back((m.p[static_cast<std::size_t>(v0)] + m.p[static_cast<std::size_t>(v1)]) * 0.5f);
        m.n.push_back(normalized(m.n[static_cast<std::size_t>(v0)] + m.n[static_cast<std::size_t>(v1)]));

        // Parent local diffs (read before overwriting).
        const Int2 D01 = m.diffs[static_cast<std::size_t>(e0)];
        const Int2 D1p = m.diffs[static_cast<std::size_t>(dedgeNext(e0))];
        const Int2 Dp0 = m.diffs[static_cast<std::size_t>(dedgePrev(e0))];
        const Int2 D0n = i2half(D01);
        Int2 Ds10{}, Ds0p{}, Dsp1{}, Dsn0{};
        if (!isBoundary) {
            Ds10 = m.diffs[static_cast<std::size_t>(e1)];
            Ds0p = m.diffs[static_cast<std::size_t>(dedgeNext(e1))];
            Dsp1 = m.diffs[static_cast<std::size_t>(dedgePrev(e1))];
            int orient = 0;
            while (orient < 4 && !i2eq(rshift90i(D01, orient), Ds10)) {
                ++orient;
            }
            Dsn0 = rshift90i(D0n, orient);
        }
        // External neighbor half-edges (read before relinking clobbers them).
        const int e0p = m.e2e[static_cast<std::size_t>(dedgePrev(e0))];
        const int e0n = m.e2e[static_cast<std::size_t>(dedgeNext(e0))];
        int e1p = -1, e1n = -1;
        if (!isBoundary) {
            e1p = m.e2e[static_cast<std::size_t>(dedgePrev(e1))];
            e1n = m.e2e[static_cast<std::size_t>(dedgeNext(e1))];
        }

        const int f2 = isBoundary ? -1 : addFace();
        const int f3 = addFace();
        const auto setFace = [&](int f, int a, int b, int c, Int2 d0, Int2 d1, Int2 d2) {
            m.tris[static_cast<std::size_t>(f)] = {a, b, c};
            m.diffs[static_cast<std::size_t>(f * 3 + 0)] = d0;
            m.diffs[static_cast<std::size_t>(f * 3 + 1)] = d1;
            m.diffs[static_cast<std::size_t>(f * 3 + 2)] = d2;
        };
        setFace(f0, vn, v0p, v0, i2sub(i2add(D01, D1p), D0n), Dp0, D0n);
        if (!isBoundary) {
            const Int2 tail = i2sub(Ds10, Dsn0);
            setFace(f1, vn, v0, v1p, Dsn0, Ds0p, i2add(Dsp1, tail));
            setFace(f2, vn, v1p, v1, i2neg(i2add(Dsp1, tail)), Dsp1, tail);
        }
        setFace(f3, vn, v1, v0p, i2sub(D01, D0n), D1p, i2sub(D0n, i2add(D01, D1p)));

        sE2E(3 * f0 + 0, 3 * f3 + 2);
        sE2E(3 * f0 + 1, e0p);
        sE2E(3 * f3 + 1, e0n);
        if (isBoundary) {
            sE2E(3 * f0 + 2, -1);
            sE2E(3 * f3 + 0, -1);
        } else {
            sE2E(3 * f0 + 2, 3 * f1 + 0);
            sE2E(3 * f1 + 1, e1n);
            sE2E(3 * f1 + 2, 3 * f2 + 0);
            sE2E(3 * f2 + 1, e1p);
            sE2E(3 * f2 + 2, 3 * f3 + 0);
        }

        schedule(f0);
        if (!isBoundary) {
            schedule(f2);
            schedule(f1);
        }
        schedule(f3);
    }
    return m;
}

// A clean compact triangle manifold over the grid vertices — QuadriFlow's
// BuildTriangleManifold state (single-level). `tv` are per-face manifold vertex
// ids; `diff` mirrors the SubMesh's per-half-edge rotated diffs; `e2e` is the
// half-edge opposite; O/N are per-manifold-vertex field position and normal.
struct ManifoldTris {
    std::vector<std::array<int, 3>> tv;
    std::vector<std::array<Int2, 3>> diff;
    std::vector<int> e2e;
    std::vector<Vec3> O;
    std::vector<Vec3> N;
};

// Half-edge opposite over compact triangles: only genuinely-manifold edges (one
// directed occurrence each way) get paired; boundary / non-manifold edges stay -1
// so the orbit walk splits them cleanly.
std::vector<int> computeTriE2E(const std::vector<std::array<int, 3>>& tris) {
    std::map<std::pair<int, int>, int> count;
    std::map<std::pair<int, int>, int> lastHe;
    for (std::size_t t = 0; t < tris.size(); ++t) {
        for (int j = 0; j < 3; ++j) {
            const std::pair<int, int> d{tris[t][static_cast<std::size_t>(j)],
                                        tris[t][static_cast<std::size_t>((j + 1) % 3)]};
            ++count[d];
            lastHe[d] = static_cast<int>(t) * 3 + j;
        }
    }
    std::vector<int> e2e(tris.size() * 3, -1);
    for (std::size_t t = 0; t < tris.size(); ++t) {
        for (int j = 0; j < 3; ++j) {
            const int a = tris[t][static_cast<std::size_t>(j)];
            const int b = tris[t][static_cast<std::size_t>((j + 1) % 3)];
            const auto fwd = count.find({a, b});
            const auto rev = count.find({b, a});
            if (fwd != count.end() && fwd->second == 1 && rev != count.end() && rev->second == 1) {
                e2e[t * 3 + static_cast<std::size_t>(j)] = lastHe[{b, a}];
            }
        }
    }
    return e2e;
}

int triPrevHe(int de) { return de / 3 * 3 + (de + 2) % 3; }
int triNextHe(int de) { return de / 3 * 3 + (de + 1) % 3; }

// The fan of directed edges around the vertex at half-edge `seed`, walked through
// the triangle opposite map `e2e`: back to the start on a closed fan, or across
// both directions from the seed on an open (boundary) fan.
std::deque<int> gatherVertexFan(const std::vector<int>& e2e, int seed) {
    std::deque<int> orbit;
    int d = seed;
    do {
        orbit.push_back(d);
        d = e2e[static_cast<std::size_t>(triPrevHe(d))];
    } while (d != -1 && d != seed);
    if (d == -1) {  // open fan: also walk the other direction from the seed
        d = seed;
        while (true) {
            d = e2e[static_cast<std::size_t>(d)];
            if (d == -1 || triNextHe(d) == seed) {
                break;
            }
            d = triNextHe(d);
            orbit.push_front(d);
        }
    }
    return orbit;
}

// Splits non-manifold manifold-vertices — a vertex whose incident-triangle fan
// revisits a directed edge, i.e. wraps into several sub-loops (a bowtie / pinch)
// — into one distinct vertex per sub-loop, until stable (QuadriFlow
// BuildTriangleManifold repair loop). Keeps tv / e2e / O / N consistent so the
// downstream boundary trace and quad pairing see a clean manifold.
void splitNonManifoldVertices(ManifoldTris& mt) {
    const auto tvAt = [&](int de) -> int& {
        return mt.tv[static_cast<std::size_t>(de / 3)][static_cast<std::size_t>(de % 3)];
    };
    const auto nextHe = [](int de) { return triNextHe(de); };

    int numV = static_cast<int>(mt.O.size());
    int prev = -1;
    while (numV != prev) {
        prev = numV;

        // Build vertex -> incident dedges, and disconnect degenerate triangles.
        std::vector<std::vector<int>> vertToDedge(static_cast<std::size_t>(numV));
        for (std::size_t i = 0; i < mt.tv.size(); ++i) {
            const auto& pt = mt.tv[i];
            if (pt[0] == pt[1] || pt[1] == pt[2] || pt[2] == pt[0]) {
                for (int j = 0; j < 3; ++j) {
                    const int t = mt.e2e[i * 3 + static_cast<std::size_t>(j)];
                    if (t != -1) {
                        mt.e2e[static_cast<std::size_t>(t)] = -1;
                    }
                }
                for (int j = 0; j < 3; ++j) {
                    mt.e2e[i * 3 + static_cast<std::size_t>(j)] = -1;
                }
            } else {
                for (int j = 0; j < 3; ++j) {
                    const int vtx = pt[static_cast<std::size_t>(j)];
                    const int he0 = static_cast<int>(i) * 3 + j;
                    vertToDedge[static_cast<std::size_t>(vtx)].push_back(he0);
                }
            }
        }

        std::vector<int> colors(mt.tv.size() * 3, -1);
        for (int i = 0; i < numV; ++i) {
            int numColor = 0;
            for (const int seed : vertToDedge[static_cast<std::size_t>(i)]) {
                if (colors[static_cast<std::size_t>(seed)] != -1) {
                    continue;
                }
                const std::deque<int> orbit = gatherVertexFan(mt.e2e, seed);
                // Split the fan wherever a directed edge (v1,v2) repeats: the run
                // between the two occurrences is a closed sub-loop -> its own color.
                std::map<std::pair<int, int>, int> loc;
                std::vector<int> dedgeColor(orbit.size(), numColor);
                ++numColor;
                for (std::size_t jj = 0; jj < orbit.size(); ++jj) {
                    const int de = orbit[jj];
                    colors[static_cast<std::size_t>(de)] = 0;
                    const std::pair<int, int> key{tvAt(de), tvAt(nextHe(de))};
                    const auto it = loc.find(key);
                    if (it != loc.end()) {
                        for (int k = it->second; k < static_cast<int>(jj); ++k) {
                            const int de1 = orbit[static_cast<std::size_t>(k)];
                            loc.erase({tvAt(de1), tvAt(nextHe(de1))});
                            dedgeColor[static_cast<std::size_t>(k)] = numColor;
                        }
                        ++numColor;
                    }
                    loc[key] = static_cast<int>(jj);
                }
                for (std::size_t j = 0; j < orbit.size(); ++j) {
                    if (dedgeColor[j] > 0) {
                        tvAt(orbit[j]) = numV + dedgeColor[j] - 1;
                    }
                }
            }
            if (numColor > 1) {
                for (int j = 0; j < numColor - 1; ++j) {
                    mt.O.push_back(mt.O[static_cast<std::size_t>(i)]);
                    mt.N.push_back(mt.N[static_cast<std::size_t>(i)]);
                }
                numV += numColor - 1;
            }
        }
    }

    // Drop triangles that stayed degenerate; compact tv + diff (e2e is unused
    // downstream — the quad pairing and FixHoles rebuild their own adjacency).
    std::size_t offset = 0;
    for (std::size_t i = 0; i < mt.tv.size(); ++i) {
        const auto& pt = mt.tv[i];
        if (pt[0] == pt[1] || pt[1] == pt[2] || pt[2] == pt[0]) {
            continue;
        }
        mt.tv[offset] = mt.tv[i];
        mt.diff[offset] = mt.diff[i];
        ++offset;
    }
    mt.tv.resize(offset);
    mt.diff.resize(offset);
}

// Reconstructs the compact triangle manifold from the subdivided unit-cell mesh
// (QuadriFlow AdvancedExtractQuad + BuildTriangleManifold, single-level):
//  1. collapse zero-diff edges into compact grid vertices (averaged O / N);
//  2. build compact triangles, dropping any that straddle a collapsed edge;
//  3. orbit-walk each vertex fan to assign a per-fan manifold vertex id — this
//     splits non-manifold vertices (multiple disconnected fans) into distinct
//     vertices so the downstream boundary tracing and quad pairing are clean.
ManifoldTris buildManifoldTris(const SubMesh& m) {
    const std::size_t nV = m.o.size();

    // 1. Zero-diff collapse -> compact grid vertices with averaged geometry.
    DisjointSets ds(nV);
    for (std::size_t he = 0; he < m.diffs.size(); ++he) {
        if (m.diffs[he].x == 0 && m.diffs[he].y == 0) {
            const std::size_t t = he / 3, j = he % 3;
            ds.unite(static_cast<Index>(m.tris[t][j]), static_cast<Index>(m.tris[t][(j + 1) % 3]));
        }
    }
    std::vector<int> compactId(nV, -1);
    int nCompact = 0;
    for (std::size_t v = 0; v < nV; ++v) {
        const Index r = ds.find(static_cast<Index>(v));
        if (compactId[r] == -1) {
            compactId[r] = nCompact++;
        }
    }
    const auto cvOf = [&](int g) { return compactId[ds.find(static_cast<Index>(g))]; };
    std::vector<Vec3> oCompact(static_cast<std::size_t>(nCompact), Vec3{});
    std::vector<Vec3> nCompactVec(static_cast<std::size_t>(nCompact), Vec3{});
    std::vector<int> cnt(static_cast<std::size_t>(nCompact), 0);
    for (std::size_t v = 0; v < nV; ++v) {
        const int c = cvOf(static_cast<int>(v));
        oCompact[static_cast<std::size_t>(c)] += m.o[v];
        nCompactVec[static_cast<std::size_t>(c)] += m.n[v];
        ++cnt[static_cast<std::size_t>(c)];
    }
    for (int c = 0; c < nCompact; ++c) {
        if (cnt[static_cast<std::size_t>(c)] > 0) {
            oCompact[static_cast<std::size_t>(c)] =
                oCompact[static_cast<std::size_t>(c)] *
                (1.0f / static_cast<float>(cnt[static_cast<std::size_t>(c)]));
        }
        nCompactVec[static_cast<std::size_t>(c)] =
            normalized(nCompactVec[static_cast<std::size_t>(c)]);
    }

    // 2. Compact triangles (distinct corners) + their rotated diffs.
    std::vector<std::array<int, 3>> ctris;
    std::vector<std::array<Int2, 3>> cdiff;
    for (std::size_t t = 0; t < m.tris.size(); ++t) {
        const int a = cvOf(m.tris[t][0]), b = cvOf(m.tris[t][1]), c = cvOf(m.tris[t][2]);
        if (a == b || b == c || a == c) {
            continue;  // straddles a collapsed edge -> degenerate
        }
        ctris.push_back({a, b, c});
        cdiff.push_back({m.diffs[t * 3 + 0], m.diffs[t * 3 + 1], m.diffs[t * 3 + 2]});
    }
    const std::vector<int> e2e = computeTriE2E(ctris);

    // 3. Orbit-walk each vertex fan into a fresh per-fan manifold vertex id.
    ManifoldTris mt;
    mt.diff = std::move(cdiff);
    mt.e2e = e2e;
    mt.tv.assign(ctris.size(), {-1, -1, -1});
    int numV = 0;
    for (std::size_t t = 0; t < ctris.size(); ++t) {
        for (int j = 0; j < 3; ++j) {
            if (mt.tv[t][static_cast<std::size_t>(j)] != -1) {
                continue;
            }
            const int cid = ctris[t][static_cast<std::size_t>(j)];
            mt.O.push_back(oCompact[static_cast<std::size_t>(cid)]);
            mt.N.push_back(nCompactVec[static_cast<std::size_t>(cid)]);
            const int d0 = static_cast<int>(t) * 3 + j;
            int d = d0;
            do {
                mt.tv[static_cast<std::size_t>(d / 3)][static_cast<std::size_t>(d % 3)] = numV;
                d = e2e[static_cast<std::size_t>(d / 3 * 3 + (d + 2) % 3)];
            } while (d != d0 && d != -1);
            if (d == -1) {  // open fan: walk the other direction from the seed
                d = d0;
                while (true) {
                    d = e2e[static_cast<std::size_t>(d)];
                    if (d == -1) {
                        break;
                    }
                    d = d / 3 * 3 + (d + 1) % 3;
                    mt.tv[static_cast<std::size_t>(d / 3)][static_cast<std::size_t>(d % 3)] = numV;
                }
            }
            ++numV;
        }
    }

    // 4. Split non-manifold vertices so the output is a clean manifold.
    splitNonManifoldVertices(mt);
    return mt;
}

// --- FixHoles: fill residual boundary loops with quads (QuadriFlow) -----------

// Quad half-edge adjacency (compute_direct_graph_quad): E2E[4*f+i] is the opposite
// of the directed edge F[f][i]->F[f][i+1], or -1 on boundary / non-manifold; V2E[v]
// is one outgoing half-edge of vertex v (-1 if unused).
struct QuadDedge {
    std::vector<int> v2e;
    std::vector<int> e2e;
};
QuadDedge quadDedge(const std::vector<std::array<int, 4>>& faces, int nVerts) {
    std::vector<int> v2e(static_cast<std::size_t>(nVerts), -1);
    const int nHe = static_cast<int>(faces.size()) * 4;
    std::vector<int> target(static_cast<std::size_t>(nHe), -1);
    std::vector<int> nextChain(static_cast<std::size_t>(nHe), -1);
    for (std::size_t f = 0; f < faces.size(); ++f) {
        for (int i = 0; i < 4; ++i) {
            const int a = faces[f][static_cast<std::size_t>(i)];
            const int b = faces[f][static_cast<std::size_t>((i + 1) % 4)];
            const int eid = static_cast<int>(f) * 4 + i;
            if (a == b) {
                continue;
            }
            target[static_cast<std::size_t>(eid)] = b;
            if (v2e[static_cast<std::size_t>(a)] == -1) {
                v2e[static_cast<std::size_t>(a)] = eid;
            } else {
                int idx = v2e[static_cast<std::size_t>(a)];
                while (nextChain[static_cast<std::size_t>(idx)] != -1) {
                    idx = nextChain[static_cast<std::size_t>(idx)];
                }
                nextChain[static_cast<std::size_t>(idx)] = eid;
            }
        }
    }
    std::vector<int> e2e(static_cast<std::size_t>(nHe), -1);
    for (std::size_t f = 0; f < faces.size(); ++f) {
        for (int i = 0; i < 4; ++i) {
            const int a = faces[f][static_cast<std::size_t>(i)];
            const int b = faces[f][static_cast<std::size_t>((i + 1) % 4)];
            const int eid = static_cast<int>(f) * 4 + i;
            if (a == b) {
                continue;
            }
            int it = v2e[static_cast<std::size_t>(b)], opp = -1;
            while (it != -1) {
                if (target[static_cast<std::size_t>(it)] == a) {
                    if (opp == -1) {
                        opp = it;
                    } else {
                        opp = -1;  // non-manifold: leave both as boundary
                        break;
                    }
                }
                it = nextChain[static_cast<std::size_t>(it)];
            }
            if (opp != -1 && eid < opp) {
                e2e[static_cast<std::size_t>(eid)] = opp;
                e2e[static_cast<std::size_t>(opp)] = eid;
            }
        }
    }
    return {std::move(v2e), std::move(e2e)};
}

// Min-angle-energy quadrangulation of a boundary loop (QuadriFlow QuadEnergy):
// a length-4 loop emits one quad and returns its squared-angle-deviation energy;
// longer even loops are split at every (seg1, seg2) parity-respecting pair and
// the minimum-energy partition is kept. `budget` bounds the exponential search.
double quadEnergy(const std::vector<int>& loop, const std::vector<Vec3>& O,
                  const std::vector<Vec3>& N, std::vector<std::array<int, 4>>& res, int& budget) {
    const int n = static_cast<int>(loop.size());
    if (n < 4) {
        return 0.0;
    }
    if (--budget < 0) {
        return 1e30;  // recursion budget exhausted: leave this (large) loop unfilled
    }
    if (n == 4) {
        double energy = 0.0;
        for (int j = 0; j < 4; ++j) {
            const int v0 = loop[static_cast<std::size_t>(j)];
            const int v2 = loop[static_cast<std::size_t>((j + 1) % 4)];
            const int v1 = loop[static_cast<std::size_t>((j + 3) % 4)];
            const Vec3 pt1 =
                normalized(O[static_cast<std::size_t>(v1)] - O[static_cast<std::size_t>(v0)]);
            const Vec3 pt2 =
                normalized(O[static_cast<std::size_t>(v2)] - O[static_cast<std::size_t>(v0)]);
            const Vec3 nn = cross(pt1, pt2);
            double sina = length(nn);
            if (dot(nn, N[static_cast<std::size_t>(v0)]) < 0.0f) {
                sina = -sina;
            }
            const double cosa = dot(pt1, pt2);
            double angle = std::atan2(sina, cosa) / static_cast<double>(kPi) * 180.0;
            if (angle < 0.0) {
                angle = 360.0 + angle;
            }
            energy += angle * angle;
        }
        res.push_back({loop[0], loop[3], loop[2], loop[1]});
        return energy;
    }
    double best = 1e30;
    for (int seg1 = 2; seg1 < n; seg1 += 2) {
        for (int seg2 = seg1 + 1; seg2 < n; seg2 += 2) {
            std::array<std::vector<std::array<int, 4>>, 4> quads;
            std::vector<int> corner{loop[0], loop[1], loop[static_cast<std::size_t>(seg1)],
                                    loop[static_cast<std::size_t>(seg2)]};
            double energy = quadEnergy(corner, O, N, quads[0], budget);
            if (seg1 > 2) {
                std::vector<int> v(loop.begin() + 1, loop.begin() + seg1);
                v.push_back(loop[static_cast<std::size_t>(seg1)]);
                energy += quadEnergy(v, O, N, quads[1], budget);
            }
            if (seg2 != seg1 + 1) {
                std::vector<int> v(loop.begin() + seg1, loop.begin() + seg2);
                v.push_back(loop[static_cast<std::size_t>(seg2)]);
                energy += quadEnergy(v, O, N, quads[2], budget);
            }
            if (seg2 + 1 != n) {
                std::vector<int> v(loop.begin() + seg2, loop.end());
                v.push_back(loop[0]);
                energy += quadEnergy(v, O, N, quads[3], budget);
            }
            if (best > energy) {
                best = energy;
                res.clear();
                for (const auto& part : quads) {
                    for (const auto& q : part) {
                        res.push_back(q);
                    }
                }
            }
        }
    }
    return best;
}

// Fill a single traced boundary loop: split it at repeated vertices into simple
// sub-loops, quadrangulate each with quadEnergy, and add the quads whose edges do
// not already exist (QuadriFlow FixHoles(loop_vertices)).
void fixHolesLoop(const std::vector<int>& loopIn, std::vector<std::array<int, 4>>& faces,
                  std::set<std::pair<int, int>>& quadEdges, const std::vector<Vec3>& O,
                  const std::vector<Vec3>& N) {
    std::vector<std::vector<int>> subLoops;
    std::unordered_map<int, int> seen;
    for (int i = 0; i < static_cast<int>(loopIn.size()); ++i) {
        const auto it = seen.find(loopIn[static_cast<std::size_t>(i)]);
        if (it != seen.end()) {
            const int j = it->second;
            subLoops.emplace_back();
            if (i - j > 3 && (i - j) % 2 == 0) {
                for (int k = j; k < i; ++k) {
                    const auto it2 = seen.find(loopIn[static_cast<std::size_t>(k)]);
                    if (it2 != seen.end()) {
                        subLoops.back().push_back(loopIn[static_cast<std::size_t>(k)]);
                        seen.erase(it2);
                    }
                }
            }
        }
        seen[loopIn[static_cast<std::size_t>(i)]] = i;
    }
    if (seen.size() >= 3) {
        subLoops.emplace_back();
        for (int k = 0; k < static_cast<int>(loopIn.size()); ++k) {
            const auto it2 = seen.find(loopIn[static_cast<std::size_t>(k)]);
            if (it2 != seen.end()) {
                subLoops.back().push_back(loopIn[static_cast<std::size_t>(k)]);
                seen.erase(it2);
            }
        }
    }
    for (auto& loop : subLoops) {
        if (loop.empty()) {
            return;  // faithful to QuadriFlow: abandons the rest
        }
        std::vector<std::array<int, 4>> quads;
        int budget = 200000;  // bounds the exponential search on pathological loops
        quadEnergy(loop, O, N, quads, budget);
        const auto quadEdge = [](const std::array<int, 4>& q, int j) {
            return std::pair<int, int>{q[static_cast<std::size_t>(j)],
                                       q[static_cast<std::size_t>((j + 1) % 4)]};
        };
        for (const auto& q : quads) {
            bool exists = false;
            for (int j = 0; j < 4; ++j) {
                if (quadEdges.count(quadEdge(q, j))) {
                    exists = true;
                    break;
                }
            }
            if (!exists) {
                for (int j = 0; j < 4; ++j) {
                    quadEdges.insert(quadEdge(q, j));
                }
                faces.push_back(q);
            }
        }
    }
}

// Trace every boundary loop of the quad mesh and fill loops under 25 vertices
// (QuadriFlow FixHoles()). Mutates `faces`; new quads reference existing vertices.
void fixHoles(std::vector<std::array<int, 4>>& faces, const std::vector<Vec3>& O,
              const std::vector<Vec3>& N, int nVerts) {
    std::set<std::pair<int, int>> quadEdges;
    for (const auto& f : faces) {
        for (int j = 0; j < 4; ++j) {
            quadEdges.insert(
                {f[static_cast<std::size_t>(j)], f[static_cast<std::size_t>((j + 1) % 4)]});
        }
    }
    const std::vector<int> e2e = quadDedge(faces, nVerts).e2e;
    std::vector<char> detected(e2e.size(), 0);
    for (int i = 0; i < static_cast<int>(e2e.size()); ++i) {
        if (detected[static_cast<std::size_t>(i)] != 0 || e2e[static_cast<std::size_t>(i)] != -1) {
            continue;
        }
        std::vector<int> loopEdges;
        int cur = i;
        bool degenerate = false;
        while (detected[static_cast<std::size_t>(cur)] == 0) {
            detected[static_cast<std::size_t>(cur)] = 1;
            loopEdges.push_back(cur);
            cur = cur / 4 * 4 + (cur + 1) % 4;
            std::size_t guard = 0;
            while (e2e[static_cast<std::size_t>(cur)] != -1) {
                cur = e2e[static_cast<std::size_t>(cur)];
                cur = cur / 4 * 4 + (cur + 1) % 4;
                if (++guard > e2e.size()) {
                    degenerate = true;  // non-manifold fan: abandon this loop
                    break;
                }
            }
            if (degenerate) {
                break;
            }
        }
        if (degenerate) {
            continue;
        }
        std::vector<int> loopV(loopEdges.size());
        for (std::size_t j = 0; j < loopEdges.size(); ++j) {
            loopV[j] = faces[static_cast<std::size_t>(loopEdges[j] / 4)]
                            [static_cast<std::size_t>(loopEdges[j] % 4)];
        }
        if (loopV.size() < 25) {
            fixHolesLoop(loopV, faces, quadEdges, O, N);
        }
    }
}

// --- FixValence: topology cleanup on the quad mesh (QuadriFlow) ----------------

int qPrevHe(int de) { return de / 4 * 4 + (de + 3) % 4; }

// One pass of doublet dissolution (QuadriFlow FixValence "Remove Valence 2"):
// each valence-2 vertex is shared by exactly two quads along two edges; merge
// those two quads into a single quad and drop the doublet vertex. Returns whether
// anything changed (the caller loops to a fixpoint).
bool removeDoubletsPass(std::vector<std::array<int, 4>>& faces, int nV) {
    const QuadDedge d = quadDedge(faces, nV);
    std::vector<char> marks(static_cast<std::size_t>(nV), 0);
    std::vector<char> erasedF(faces.size(), 0);
    const auto corner = [&](int de, int off) {
        return faces[static_cast<std::size_t>(de / 4)][static_cast<std::size_t>((de + off) % 4)];
    };
    bool update = false;
    for (int i = 0; i < nV; ++i) {
        const int deid0 = d.v2e[static_cast<std::size_t>(i)];
        if (marks[static_cast<std::size_t>(i)] || deid0 == -1) {
            continue;
        }
        int deid = deid0;
        std::vector<int> dedges;
        const std::size_t fanCap = d.e2e.size() + 1;
        do {
            dedges.push_back(deid);
            deid = d.e2e[static_cast<std::size_t>(qPrevHe(deid))];
        } while (deid != deid0 && deid != -1 && dedges.size() <= fanCap);
        // A true doublet is an *interior* valence-2 vertex: its fan closes back to
        // the seed. A boundary valence-2 vertex (fan hits -1) is not a doublet —
        // merging it opens seams — so skip it.
        if (deid != deid0 || dedges.size() != 2) {
            continue;
        }
        const int v1 = corner(dedges[0], 1), v2 = corner(dedges[0], 2);
        const int v3 = corner(dedges[1], 1), v4 = corner(dedges[1], 2);
        if (marks[static_cast<std::size_t>(v1)] || marks[static_cast<std::size_t>(v2)] ||
            marks[static_cast<std::size_t>(v3)] || marks[static_cast<std::size_t>(v4)]) {
            continue;
        }
        marks[static_cast<std::size_t>(v1)] = 1;
        marks[static_cast<std::size_t>(v2)] = 1;
        marks[static_cast<std::size_t>(v3)] = 1;
        marks[static_cast<std::size_t>(v4)] = 1;
        if (v1 == v2 || v1 == v3 || v1 == v4 || v2 == v3 || v2 == v4 || v3 == v4) {
            erasedF[static_cast<std::size_t>(dedges[0] / 4)] = 1;
        } else {
            faces[static_cast<std::size_t>(dedges[0] / 4)] = {v1, v2, v3, v4};
        }
        erasedF[static_cast<std::size_t>(dedges[1] / 4)] = 1;
        update = true;
    }
    if (update) {
        std::size_t top = 0;
        for (std::size_t f = 0; f < faces.size(); ++f) {
            if (!erasedF[f]) {
                faces[top++] = faces[f];
            }
        }
        faces.resize(top);
    }
    return update;
}

// Topology cleanup on the extracted quad mesh (QuadriFlow FixValence, "Remove
// Valence 2"): dissolve interior valence-2 doublets to a fixpoint. Restricted to
// interior doublets — merging a boundary valence-2 vertex opens seams and was
// measured net-negative (irregular + boundary both up); the high-valence split /
// decrease passes are likewise net-negative on this extractor and omitted.
// Geometry is untouched; freed vertices drop out at materialisation.
void fixValence(std::vector<std::array<int, 4>>& faces, int nV) {
    while (removeDoubletsPass(faces, nV)) {
    }
}

// Collapses the unit-cell mesh into a watertight quad mesh (QuadriFlow
// AdvancedExtractQuad + BuildTriangleManifold + FixValence, single-level):
// reconstruct a clean compact triangle manifold, pair the two triangles across
// each |diff|==(1,1) grid-cell diagonal into a quad, drop degenerate quads,
// FixHoles the residual boundary loops, then FixValence (dissolve doublets).
// Output vertex position is the mean field position (O).
Mesh buildQuadMesh(const SubMesh& m) {
    const ManifoldTris mt = buildManifoldTris(m);

    // Pair the two triangles across each grid-cell diagonal (QuadriFlow keys the
    // diagonal by its manifold-vertex endpoint pair).
    std::map<std::pair<int, int>, std::pair<std::array<int, 3>, std::array<int, 3>>> quads;
    for (std::size_t t = 0; t < mt.tv.size(); ++t) {
        for (int j = 0; j < 3; ++j) {
            const Int2 d = mt.diff[t][static_cast<std::size_t>(j)];
            if (std::abs(d.x) != 1 || std::abs(d.y) != 1) {
                continue;
            }
            const int v1 = mt.tv[t][static_cast<std::size_t>(j)];
            const int v2 = mt.tv[t][static_cast<std::size_t>((j + 1) % 3)];
            const int v3 = mt.tv[t][static_cast<std::size_t>((j + 2) % 3)];
            const std::pair<int, int> key =
                v1 < v2 ? std::make_pair(v1, v2) : std::make_pair(v2, v1);
            const auto it = quads.find(key);
            if (it == quads.end()) {
                quads[key] = {{v1, v2, v3}, {-1, -1, -1}};
            } else {
                it->second.second = {v1, v2, v3};
            }
        }
    }
    std::vector<std::array<int, 4>> faces;
    for (const auto& kv : quads) {
        const auto& f = kv.second.first;
        const auto& s = kv.second.second;
        if (s[0] != -1 && f[2] != s[2]) {
            faces.push_back({f[1], f[2], f[0], s[2]});
        }
    }

    // Drop degenerate quads (repeated vertex) — removing one cannot create another.
    std::vector<std::array<int, 4>> cleaned;
    cleaned.reserve(faces.size());
    for (const auto& q : faces) {
        bool distinct = true;
        for (int a = 0; a < 3 && distinct; ++a) {
            for (int b = a + 1; b < 4; ++b) {
                if (q[static_cast<std::size_t>(a)] == q[static_cast<std::size_t>(b)]) {
                    distinct = false;
                    break;
                }
            }
        }
        if (distinct) {
            cleaned.push_back(q);
        }
    }
    faces.swap(cleaned);

    fixHoles(faces, mt.O, mt.N, static_cast<int>(mt.O.size()));
    fixValence(faces, static_cast<int>(mt.O.size()));

    // Materialise: only manifold vertices referenced by a face become mesh verts.
    Mesh out;
    std::map<int, VertexId> remap;
    const auto getV = [&](int v) {
        const auto it = remap.find(v);
        if (it != remap.end()) {
            return it->second;
        }
        const VertexId id = out.addVertex(mt.O[static_cast<std::size_t>(v)]);
        remap.emplace(v, id);
        return id;
    };
    for (const auto& q : faces) {
        const std::array<VertexId, 4> vv{getV(q[0]), getV(q[1]), getV(q[2]), getV(q[3])};
        out.addFace(vv);
    }
    return out;
}

// --- Phase 5d glue: reconstruct an IntegerGrid from the subdivided unit-cell mesh
// so the flip-repair SAT can run on the *unit* diffs (its ternary domain requires
// |component| <= 1, which only holds post-subdivide). The SubMesh's per-half-edge
// `diffs` are already the rotated local-frame diffs, so pair opposite half-edges
// via `e2e` into undirected edges: the representative's diff is canonical
// (orient 0), and the opposite's orient is the rotation k with
// rshift90i(canonical, k) == its diff (a k always exists — negation is rotation
// by 2). faceArea / loop-closure of the reconstructed grid match the SubMesh.
IntegerGrid integerGridFromSubMesh(const SubMesh& m) {
    IntegerGrid g;
    const int nTri = static_cast<int>(m.tris.size());
    g.tris.resize(static_cast<std::size_t>(nTri));
    for (int t = 0; t < nTri; ++t) {
        g.tris[static_cast<std::size_t>(t)] = {static_cast<Index>(m.tris[static_cast<std::size_t>(t)][0]),
                                               static_cast<Index>(m.tris[static_cast<std::size_t>(t)][1]),
                                               static_cast<Index>(m.tris[static_cast<std::size_t>(t)][2])};
    }
    g.e2e = m.e2e;
    g.faceEdgeIds.assign(static_cast<std::size_t>(nTri), {-1, -1, -1});
    g.orients.assign(static_cast<std::size_t>(nTri), {0, 0, 0});
    for (int he = 0; he < nTri * 3; ++he) {
        const std::size_t t = static_cast<std::size_t>(he / 3), j = static_cast<std::size_t>(he % 3);
        if (g.faceEdgeIds[t][j] != -1) {
            continue;
        }
        const int eid = static_cast<int>(g.edgeDiff.size());
        g.edgeDiff.push_back(m.diffs[static_cast<std::size_t>(he)]);
        g.faceEdgeIds[t][j] = eid;
        g.orients[t][j] = 0;
        const int opp = m.e2e[static_cast<std::size_t>(he)];
        if (opp != -1) {
            const std::size_t ot = static_cast<std::size_t>(opp / 3), oj = static_cast<std::size_t>(opp % 3);
            g.faceEdgeIds[ot][oj] = eid;
            int k = 0;
            for (; k < 4; ++k) {
                const Int2 r = rshift90i(g.edgeDiff[static_cast<std::size_t>(eid)], k);
                if (r.x == m.diffs[static_cast<std::size_t>(opp)].x &&
                    r.y == m.diffs[static_cast<std::size_t>(opp)].y) {
                    break;
                }
            }
            g.orients[ot][oj] = (k < 4) ? k : 0;
        }
    }
    return g;
}

// Recompute the SubMesh's per-half-edge diffs from a (possibly SAT-repaired)
// IntegerGrid — the inverse of the pairing above. With no repair this is an
// identity on `m.diffs`.
void writeDiffsBackToSubMesh(const IntegerGrid& g, SubMesh& m) {
    for (int he = 0; he < static_cast<int>(m.diffs.size()); ++he) {
        const std::size_t t = static_cast<std::size_t>(he / 3), j = static_cast<std::size_t>(he % 3);
        m.diffs[static_cast<std::size_t>(he)] =
            rshift90i(g.edgeDiff[static_cast<std::size_t>(g.faceEdgeIds[t][j])], g.orients[t][j]);
    }
}

}  // namespace

Mesh extractIntegerQuadMesh(const Mesh& mesh, const PositionField& field) {
    IntegerGrid g = computeIntegerGrid(mesh, field);
    fixFlipHierarchy(g);  // Phase 5b: greedy flip repair (non-unit grid OK)
    SubMesh m = subdivideToUnitCells(g, mesh, field, 1);
    // Phase 5d: SAT flip repair needs the UNIT-diff grid — reconstruct it from the
    // subdivided mesh, clear the residual multi-cell flip clusters, write back.
    IntegerGrid gs = integerGridFromSubMesh(m);
    fixFlipSatHierarchy(gs);
    writeDiffsBackToSubMesh(gs, m);
    return buildQuadMesh(m);
}

IntegerExtractStats debugIntegerExtract(const Mesh& mesh, const PositionField& field) {
    const Mesh q = extractIntegerQuadMesh(mesh, field);
    IntegerExtractStats st;
    st.quads = q.faceCount();
    st.verts = q.vertexCount();
    for (Index fi = 0; fi < q.faceCapacity(); ++fi) {
        const FaceId f{fi};
        if (q.isAlive(f) && q.faceSize(f) != 4) {
            ++st.nonQuad;
        }
    }
    for (Index vi = 0; vi < q.vertexCapacity(); ++vi) {
        const VertexId v{vi};
        if (!q.isAlive(v)) {
            continue;
        }
        if (q.vertexFaces(v).size() != 4) {
            ++st.irregular;
        }
    }
    for (Index ei = 0; ei < q.edgeCapacity(); ++ei) {
        const EdgeId e{ei};
        if (q.isAlive(e) && q.isBoundaryEdge(e)) {
            ++st.boundaryEdges;
        }
    }
    return st;
}

SubdivideStats debugSubdivide(const Mesh& mesh, const PositionField& field) {
    const IntegerGrid g = computeIntegerGrid(mesh, field);
    const SubMesh m = subdivideToUnitCells(g, mesh, field, 1);
    SubdivideStats st;
    st.trisBefore = g.tris.size();
    st.trisAfter = m.tris.size();
    st.vertsAfter = m.o.size();
    st.maxDiff = 0;
    for (const Int2 d : m.diffs) {
        st.maxDiff = std::max(st.maxDiff, i2maxComp(d));
    }
    st.residualBefore = countResidualSingularities(g);
    // A subdivided triangle is integrable iff its three local-frame diffs sum to
    // zero; a nonzero sum is a spurious singularity injected by subdivision.
    for (std::size_t t = 0; t < m.tris.size(); ++t) {
        const Int2 s = i2add(i2add(m.diffs[t * 3 + 0], m.diffs[t * 3 + 1]), m.diffs[t * 3 + 2]);
        if (s.x != 0 || s.y != 0) {
            ++st.residualAfter;
        }
    }
    return st;
}

FlipRepairStats debugFlipRepair(const Mesh& mesh, const PositionField& field) {
    IntegerGrid g = computeIntegerGrid(mesh, field);
    FlipRepairStats st;
    st.faces = g.tris.size();
    st.flippedBefore = countFlipped(g);
    st.residualBefore = countResidualSingularities(g);

    // Round-trip WITHOUT repair: coarsen + propagate must be an identity on edge_diff.
    const std::vector<Int2> before = g.edgeDiff;
    {
        EdgeHierarchy h;
        std::vector<int> allow(g.edgeDiff.size() * 2, 1);
        h.downsample(g.orients, g.faceEdgeIds, g.edgeDiff, allow, -1);
        h.propagateEdge();
        st.levels = h.levels;
        std::vector<std::array<int, 3>> fq, f2e;
        std::vector<Int2> ediff;
        h.updateGraphValue(fq, f2e, ediff);
        for (std::size_t i = 0; i < before.size() && i < ediff.size(); ++i) {
            if (before[i].x != ediff[i].x || before[i].y != ediff[i].y) {
                ++st.roundTripMismatch;
            }
        }
    }

    fixFlipHierarchy(g);  // 5a: identity; 5b: real repair
    st.flippedAfter = countFlipped(g);
    st.residualAfter = countResidualSingularities(g);
    return st;
}

std::size_t debugSubMeshDiffRoundTrip(const Mesh& mesh, const PositionField& field) {
    IntegerGrid g = computeIntegerGrid(mesh, field);
    fixFlipHierarchy(g);
    SubMesh m = subdivideToUnitCells(g, mesh, field, 1);
    const std::vector<Int2> before = m.diffs;
    const IntegerGrid gs = integerGridFromSubMesh(m);
    writeDiffsBackToSubMesh(gs, m);  // no repair -> must be an exact identity
    std::size_t mismatch = 0;
    for (std::size_t i = 0; i < before.size(); ++i) {
        if (before[i].x != m.diffs[i].x || before[i].y != m.diffs[i].y) {
            ++mismatch;
        }
    }
    return mismatch;
}

bool debugTernaryCsp() {
    // Case A — a face that is loop-closed (v0+v1+v2==0) but flipped (area < 0);
    // the solver must find a satisfying, non-flipped ternary assignment.
    {
        std::vector<int> value{1, 0, -1, -1};
        const std::vector<char> flex{1, 1, 1, 1};
        std::vector<EqRow> eqs{{0, 1, 2, 1, 1, 1}};
        std::vector<GeRow> ges{{0, 1, 2, 3, 1, 1}};
        if (solveTernaryCsp(value, flex, eqs, ges, 50000) != SatStatus::Sat) {
            return false;
        }
        if (value[0] + value[1] + value[2] != 0) {
            return false;
        }
        if (value[0] * value[1] - value[2] * value[3] < 0) {
            return false;
        }
    }
    // Case B — all variables pinned to a loop-open assignment: UNSAT, unchanged.
    {
        std::vector<int> value{1, 1, 1, 0};
        const std::vector<int> before = value;
        const std::vector<char> flex{0, 0, 0, 0};
        std::vector<EqRow> eqs{{0, 1, 2, 1, 1, 1}};  // 1+1+1 != 0
        if (solveTernaryCsp(value, flex, eqs, {}, 50000) != SatStatus::Unsat) {
            return false;
        }
        if (value != before) {
            return false;
        }
    }
    // Case C — already satisfied: SAT with the current assignment preserved.
    {
        std::vector<int> value{0, 0, 0, 0};
        const std::vector<int> before = value;
        const std::vector<char> flex{1, 1, 1, 1};
        std::vector<EqRow> eqs{{0, 1, 2, 1, 1, 1}};
        std::vector<GeRow> ges{{0, 1, 2, 3, 1, 1}};
        if (solveTernaryCsp(value, flex, eqs, ges, 50000) != SatStatus::Sat) {
            return false;
        }
        if (value != before) {
            return false;  // minimal-change bias keeps a valid assignment as-is
        }
    }
    // Case D — a real search with a zero node budget times out (value untouched).
    {
        std::vector<int> value{0, 0, 0};
        const std::vector<int> before = value;
        const std::vector<char> flex{1, 1, 1};
        if (solveTernaryCsp(value, flex, {}, {}, 0) != SatStatus::Timeout) {
            return false;
        }
        if (value != before) {
            return false;
        }
    }
    return true;
}

bool debugMinCostFlow() {
    // Case 1: two disjoint s->t paths, cheap (cost 2) and expensive (cost 6);
    // max flow 2 must use both, min cost = 8.
    {
        MinCostFlow f;
        f.init(4);
        f.addEdge(0, 1, 1, 1);
        f.addEdge(1, 3, 1, 1);
        f.addEdge(0, 2, 1, 5);
        f.addEdge(2, 3, 1, 1);
        const auto [flow, cost] = f.solve(0, 3);
        if (flow != 2 || cost != 8) {
            return false;
        }
    }
    // Case 2: a cheaper detour must be preferred. 0->1 cost1 cap2; 1->2 cost1
    // cap1; 1->3 cost5 cap2; 2->3 cost1 cap1. Max flow 2: one unit via the cheap
    // 0-1-2-3 (cost 3), one via 0-1-3 (cost 6) -> total 9.
    {
        MinCostFlow f;
        f.init(4);
        f.addEdge(0, 1, 2, 1);
        f.addEdge(1, 2, 1, 1);
        f.addEdge(2, 3, 1, 1);
        f.addEdge(1, 3, 2, 5);
        const auto [flow, cost] = f.solve(0, 3);
        if (flow != 2 || cost != 9) {
            return false;
        }
    }
    return true;
}

IntegerConsistency measureIntegerConsistency(const Mesh& mesh, const PositionField& field) {
    IntegerConsistency r;

    // Rotate an integer 2-D vector by k * 90 degrees.
    const auto rot90 = [](Int2 t, int k) {
        for (int i = 0; i < (k & 3); ++i) {
            t = Int2{-t.y, t.x};
        }
        return t;
    };
    // The u->v field connection: 4-RoSy rotation index (0..3) and integer
    // translation, from the same orientation / lattice-position matching the
    // collapse uses.
    const auto connection = [&](Index u, Index v, int& rotOut, Int2& tOut) {
        // Rotation index: pick the aligned representative pair (index ba for u,
        // bb for v) as compatOrientation does, but keep the indices. The transition
        // rotation is (bb - ba), plus 2 (a 180-degree turn) when the match needed a
        // sign flip. This is the 90-degree turn count taking u's frame to v's.
        const Vec3 nu = field.normal[u], nv = field.normal[v];
        const std::array<Vec3, 4> a{field.q[u], cross(nu, field.q[u]),
                                    field.q[u] * -1.0f, cross(nu, field.q[u]) * -1.0f};
        const std::array<Vec3, 4> b{field.q[v], cross(nv, field.q[v]),
                                    field.q[v] * -1.0f, cross(nv, field.q[v]) * -1.0f};
        int ba = 0, bb = 0;
        float best = -2.0f;
        for (int i = 0; i < 4; ++i) {
            for (int j = 0; j < 4; ++j) {
                const float dt = dot(a[static_cast<std::size_t>(i)], b[static_cast<std::size_t>(j)]);
                if (dt > best) {
                    best = dt;
                    ba = i;
                    bb = j;
                }
            }
        }
        rotOut = (bb - ba) & 3;
        const Vec3 q0 = a[static_cast<std::size_t>(ba)];
        const Vec3 q1 = b[static_cast<std::size_t>(bb)];
        const float sU = field.scale.empty() ? field.spacing : field.spacing * field.scale[u];
        const float sV = field.scale.empty() ? field.spacing : field.spacing * field.scale[v];
        const float s = 0.5f * (sU + sV);
        const PosCompat pc =
            compatPosition(mesh.position(VertexId{u}), field.normal[u], q0, field.o[u],
                           mesh.position(VertexId{v}), field.normal[v], q1, field.o[v], s, 1.0f / s);
        tOut = Int2{pc.i1.x - pc.i0.x, pc.i1.y - pc.i0.y};
    };

    // Per-FACE holonomy: a triangle is always contractible, so accumulating the
    // connection (rotation + translation) around its three edges must return to
    // the identity (rotation 0, translation 0) UNLESS the face contains a true
    // field singularity. This avoids the non-contractible-loop confound of a
    // spanning-tree walk (e.g. a cylinder's circumference legitimately wraps).
    // The fraction of clean faces measures how ready the field is for the
    // Stage-2 integer solve; the singular faces are its singularities.
    double defSum = 0.0;
    std::size_t clean = 0;
    for (Index fi = 0; fi < mesh.faceCapacity(); ++fi) {
        const FaceId f{fi};
        if (!mesh.isAlive(f) || mesh.faceSize(f) != 3) {
            continue;
        }
        const auto verts = mesh.faceVertices(f);
        if (!field.valid[verts[0].value] || !field.valid[verts[1].value] ||
            !field.valid[verts[2].value]) {
            continue;
        }
        int g = 0;
        Int2 c{0, 0};
        for (std::size_t k = 0; k < 3; ++k) {
            const Index u = verts[k].value, v = verts[(k + 1) % 3].value;
            int rot = 0;
            Int2 t{};
            connection(u, v, rot, t);
            const Int2 rt = rot90(t, g);
            c = Int2{c.x + rt.x, c.y + rt.y};
            g = (g + rot) & 3;
        }
        ++r.loopEdges;  // faces tested
        ++r.vertices;
        const bool rotOk = (g == 0);
        const int d1 = std::abs(c.x) + std::abs(c.y);
        if (!rotOk) {
            ++r.rotSingular;
        }
        if (d1 != 0) {
            ++r.transDefect;
        }
        if (rotOk && d1 == 0) {
            ++clean;
        }
        defSum += static_cast<double>(d1);
    }
    r.meanDefect = r.loopEdges ? defSum / static_cast<double>(r.loopEdges) : 0.0;
    r.closedFraction = r.loopEdges ? static_cast<double>(clean) / static_cast<double>(r.loopEdges) : 1.0;
    return r;
}

}  // namespace cyber::remesh
