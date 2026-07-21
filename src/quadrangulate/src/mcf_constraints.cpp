#include "cyber/quadrangulate/mcf_layout.hpp"

#include <queue>
#include <utility>
#include <vector>

#include "mcf_detail.hpp"

// M3a of docs/mcf-integer-layout-plan.md: the integer-constraint graph — a faithful
// port of the setup half of QuadriFlow's BuildIntegerConstraints (parametrizer-int.cpp).
// Produces, per compact face, the edge orientations that align shared edges into one
// consistent frame (via a disjoint-set "orient tree"), the undirected->directed edge
// map, the connected components of non-fixed edges, and each component's net integer
// residual that the M3 flow must zero out. Pure integer math — no SciPP.
namespace cyber::remesh {

namespace {

using namespace cyber::remesh::mcf;

// QuadriFlow's DisajointOrientTree (disajoint-tree.hpp): union-find whose links also
// carry a mod-4 relative orientation, so a whole face patch can be rotated into one
// consistent frame.
class OrientTree {
   public:
    explicit OrientTree(int n) : parent_(uz(n)), rank_(uz(n), 1) {
        for (int i = 0; i < n; ++i) {
            parent_[uz(i)] = {i, 0};
        }
    }
    int parent(int j) {
        if (j == parent_[uz(j)].first) {
            return j;
        }
        const int k = parent(parent_[uz(j)].first);
        parent_[uz(j)].second =
            (parent_[uz(j)].second + parent_[uz(parent_[uz(j)].first)].second) % 4;
        parent_[uz(j)].first = k;
        return k;
    }
    int orient(int j) {
        if (j == parent_[uz(j)].first) {
            return parent_[uz(j)].second;
        }
        return (parent_[uz(j)].second + orient(parent_[uz(j)].first)) % 4;
    }
    void merge(int v0, int v1, int o0, int o1) {
        const int p0 = parent(v0), p1 = parent(v1);
        if (p0 == p1) {
            return;
        }
        const int op0 = orient(v0), op1 = orient(v1);
        if (rank_[uz(p1)] < rank_[uz(p0)]) {
            rank_[uz(p0)] += rank_[uz(p1)];
            parent_[uz(p1)] = {p0, (o1 - o0 + op0 - op1 + 8) % 4};
        } else {
            rank_[uz(p0)] += rank_[uz(p1)];
            parent_[uz(p0)] = {p1, (o0 - o1 + op1 - op0 + 8) % 4};
        }
    }

   private:
    std::vector<std::pair<int, int>> parent_;  // (parent index, relative orient mod 4)
    std::vector<int> rank_;
};

}  // namespace

McfConstraints buildMcfConstraints(const Mesh& mesh, const PositionField& field,
                                   const McfEdgeInfo& info) {
    McfConstraints out;
    if (!info.valid || info.faceList.empty()) {
        return out;
    }
    const int m = static_cast<int>(info.faceList.size());
    const int nE = static_cast<int>(info.edgeDiff.size());

    // Orientation frames (post-flip q from M2, area-weighted normal from the field).
    std::vector<D3> Q(mesh.vertexCapacity()), Nn(mesh.vertexCapacity());
    for (int f = 0; f < m; ++f) {
        for (int k = 0; k < 3; ++k) {
            const Index v = info.faceVerts[uz(f)][uz(k)];
            Q[v] = normalize(d3(info.orient[v]));
            Nn[v] = d3(field.normal[v]);
        }
    }

    // 1. Per-face edge orientations + the undirected->directed edge map (E2D).
    out.faceEdgeOrients.assign(uz(m), {0, 0, 0});
    out.e2d.assign(uz(nE), {-1, -1});
    for (int i = 0; i < m; ++i) {
        const Index v0 = info.faceVerts[uz(i)][0], v1 = info.faceVerts[uz(i)][1],
                    v2 = info.faceVerts[uz(i)][2];
        const auto idx1 = orientIndex4(Q[v0], Nn[v0], Q[v1], Nn[v1]);
        const auto idx2 = orientIndex4(Q[v0], Nn[v0], Q[v2], Nn[v2]);
        const int rank1 = (idx1.first - idx1.second + 4) % 4;  // v1 -> v0
        const int rank2 = (idx2.first - idx2.second + 4) % 4;  // v2 -> v0
        int orients[3] = {0, 0, 0};
        orients[0] = (v1 < v0) ? (rank1 + 2) % 4 : 0;
        orients[1] = (v2 < v1) ? (rank2 + 2) % 4 : rank1;
        orients[2] = (v2 < v0) ? rank2 : 2;
        out.faceEdgeOrients[uz(i)] = {orients[0], orients[1], orients[2]};
        for (int j = 0; j < 3; ++j) {
            const int e = info.faceEdgeIds[uz(i)][uz(j)];
            if (out.e2d[uz(e)].first == -1) {
                out.e2d[uz(e)].first = i * 3 + j;
            } else {
                out.e2d[uz(e)].second = i * 3 + j;
            }
        }
    }

    // Sharp (fixed) edges: feature/crease edges — the grid must break along them, so
    // they are never merged into the orientation component.
    std::vector<char> sharpUE(uz(nE), 0);
    for (int e = 0; e < nE; ++e) {
        const auto [a, b] = info.edgeValues[uz(e)];
        const EdgeId me = mesh.edgeBetween(VertexId{a}, VertexId{b});
        if (me.valid() && mesh.isFeatureEdge(me)) {
            sharpUE[uz(e)] = 1;
        }
    }
    const auto isSingular = [&](int f) {
        return info.orientSingularities.find(f) != info.orientSingularities.end();
    };

    // 2. Merge faces into one orientation frame: non-singular non-sharp edges first,
    //    then edges incident to singularities, then sharp edges (QuadriFlow's order).
    OrientTree tree(m);
    const auto mergeEdge = [&](int e) {
        const auto& ec = out.e2d[uz(e)];
        if (ec.first == -1 || ec.second == -1) {
            return;
        }
        const int f0 = ec.first / 3, f1 = ec.second / 3;
        const int orient1 = out.faceEdgeOrients[uz(f0)][uz(ec.first % 3)];
        const int orient0 = (out.faceEdgeOrients[uz(f1)][uz(ec.second % 3)] + 2) % 4;
        tree.merge(f0, f1, orient0, orient1);
    };
    for (int e = 0; e < nE; ++e) {
        const auto& ec = out.e2d[uz(e)];
        if (ec.first == -1 || ec.second == -1) {
            continue;
        }
        if (isSingular(ec.first / 3) || isSingular(ec.second / 3) || sharpUE[uz(e)]) {
            continue;
        }
        mergeEdge(e);
    }
    for (const auto& [f, unused] : info.orientSingularities) {
        for (int i = 0; i < 3; ++i) {
            const int e = info.faceEdgeIds[uz(f)][uz(i)];
            if (!sharpUE[uz(e)]) {
                mergeEdge(e);
            }
        }
    }
    for (int e = 0; e < nE; ++e) {
        if (sharpUE[uz(e)]) {
            mergeEdge(e);
        }
    }

    // Rotate every face's edge orientations into its component's parent frame.
    for (int i = 0; i < m; ++i) {
        const int rot = tree.orient(i);
        for (int j = 0; j < 3; ++j) {
            out.faceEdgeOrients[uz(i)][uz(j)] = (out.faceEdgeOrients[uz(i)][uz(j)] + rot) % 4;
        }
    }

    // 3. Label connected components joined by non-fixed edges (directed orientations
    //    differ by 2 mod 4 and not sharp) — each needs its own flow conservation.
    out.sharpColor.assign(uz(m), -1);
    out.numComponents = 0;
    for (int i = 0; i < m; ++i) {
        if (out.sharpColor[uz(i)] != -1) {
            continue;
        }
        out.sharpColor[uz(i)] = out.numComponents;
        std::queue<int> q;
        q.push(i);
        while (!q.empty()) {
            const int v = q.front();
            q.pop();
            for (int k = 0; k < 3; ++k) {
                const int e = info.faceEdgeIds[uz(v)][uz(k)];
                const int d1 = out.e2d[uz(e)].first, d2 = out.e2d[uz(e)].second;
                if (d1 == -1 || d2 == -1) {
                    continue;
                }
                const int o1 = out.faceEdgeOrients[uz(d1 / 3)][uz(d1 % 3)];
                const int o2 = out.faceEdgeOrients[uz(d2 / 3)][uz(d2 % 3)];
                if (((o1 - o2 + 4) % 4 != 2) || sharpUE[uz(e)]) {
                    continue;
                }
                for (int kk = 0; kk < 2; ++kk) {
                    const int f = (kk == 0 ? d1 : d2) / 3;
                    if (out.sharpColor[uz(f)] == -1) {
                        out.sharpColor[uz(f)] = out.numComponents;
                        q.push(f);
                    }
                }
            }
        }
        ++out.numComponents;
    }

    // 4. Net integer residual per component: the total the flow must cancel.
    out.totalFlow.assign(uz(out.numComponents), 0);
    for (int i = 0; i < m; ++i) {
        Vec2i diff{0, 0};
        for (int j = 0; j < 3; ++j) {
            const Vec2i d = rshift90(info.edgeDiff[uz(info.faceEdgeIds[uz(i)][uz(j)])],
                                     out.faceEdgeOrients[uz(i)][uz(j)]);
            diff.x += d.x;
            diff.y += d.y;
        }
        out.totalFlow[uz(out.sharpColor[uz(i)])] += diff.x + diff.y;
    }

    out.valid = true;
    return out;
}

}  // namespace cyber::remesh
