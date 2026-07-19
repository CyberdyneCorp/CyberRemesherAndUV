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
#include <map>
#include <numeric>
#include <set>
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
