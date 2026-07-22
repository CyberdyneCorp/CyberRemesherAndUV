#include <algorithm>
#include <cmath>
#include <limits>
#include <map>

#include "cyber/core/math.hpp"
#include "cyber/quadrangulate/mcf_layout.hpp"
#include "mcf_detail.hpp"

// M2 of docs/mcf-integer-layout-plan.md: build QuadriFlow's per-edge integer
// difference (`edge_diff`) from the position field. This is a faithful port of
// QuadriFlow's ComputeOrientationSingularities / ComputePositionSingularities
// (parametrizer-sing.cpp) and BuildEdgeInfo (parametrizer-int.cpp), retargeted
// onto our Mesh + PositionField. Pure integer/vector math — no SciPP.
//
// All geometry is done in double (QuadriFlow uses double throughout): the lattice
// floor-index is sensitive to rounding, and our fields are stored as float.
namespace cyber::remesh {

namespace {

using namespace cyber::remesh::mcf;  // D3, rshift90, rotate90by, orientIndex4, modulo, uz

// field-math.hpp middle_point: the point minimizing distance to both tangent planes.
D3 middlePoint(D3 p0, D3 n0, D3 p1, D3 n1) {
    const double n0p0 = dot(n0, p0), n0p1 = dot(n0, p1), n1p0 = dot(n1, p0), n1p1 = dot(n1, p1),
                 n0n1 = dot(n0, n1), denom = 1.0 / (1.0 - n0n1 * n0n1 + 1e-4),
                 l0 = 2.0 * (n0p1 - n0p0 - n0n1 * (n1p0 - n1p1)) * denom,
                 l1 = 2.0 * (n1p0 - n1p1 - n0n1 * (n0p1 - n0p0)) * denom;
    return (p0 + p1) * 0.5 - (n0 * l0 + n1 * l1) * 0.25;
}

// field-math.hpp position_floor_index_4: integer lattice coord of `o` relative to `p`.
Vec2i floorIndex4(D3 o, D3 q, D3 n, D3 p, double invSx, double invSy) {
    const D3 t = cross(n, q);
    const D3 d = p - o;
    return {static_cast<int>(std::floor(dot(q, d) * invSx)),
            static_cast<int>(std::floor(dot(t, d) * invSy))};
}

// field-math.hpp compat_position_extrinsic_index_4: the integer lattice indices of
// two adjacent lattice points; their difference is the edge's cell count.
std::pair<Vec2i, Vec2i> positionIndex4(D3 p0, D3 n0, D3 q0, D3 o0, D3 p1, D3 n1, D3 q1, D3 o1,
                                       double sx, double sy, double invSx, double invSy) {
    const D3 t0 = cross(n0, q0), t1 = cross(n1, q1);
    const D3 middle = middlePoint(p0, n0, p1, n1);
    const Vec2i o0p = floorIndex4(o0, q0, n0, middle, invSx, invSy);
    const Vec2i o1p = floorIndex4(o1, q1, n1, middle, invSx, invSy);
    double best = std::numeric_limits<double>::infinity();
    int bi = 0, bj = 0;
    for (int i = 0; i < 4; ++i) {
        const D3 o0t = o0 + q0 * (((i & 1) + o0p.x) * sx) + t0 * ((((i & 2) >> 1) + o0p.y) * sy);
        for (int j = 0; j < 4; ++j) {
            const D3 o1t =
                o1 + q1 * (((j & 1) + o1p.x) * sx) + t1 * ((((j & 2) >> 1) + o1p.y) * sy);
            const D3 e = o0t - o1t;
            const double c = dot(e, e);
            if (c < best) {
                best = c;
                bi = i;
                bj = j;
            }
        }
    }
    const Vec2i a{(bi & 1) + o0p.x, ((bi & 2) >> 1) + o0p.y};
    const Vec2i b{(bj & 1) + o1p.x, ((bj & 2) >> 1) + o1p.y};
    return {a, b};
}

// Directed-edge opposite map (QuadriFlow's mE2E): for compact face f corner k, the
// opposite directed half-edge id (f'*3+k'), or -1 on a boundary.
std::vector<int> buildE2E(const std::vector<std::array<Index, 3>>& tris) {
    std::map<std::pair<Index, Index>, int> byDir;
    const int m = static_cast<int>(tris.size());
    std::vector<int> e2e(static_cast<std::size_t>(m) * 3, -1);
    for (int f = 0; f < m; ++f) {
        for (int k = 0; k < 3; ++k) {
            const Index a = tris[uz(f)][uz(k)], b = tris[uz(f)][uz((k + 1) % 3)];
            byDir[{a, b}] = f * 3 + k;
        }
    }
    for (int f = 0; f < m; ++f) {
        for (int k = 0; k < 3; ++k) {
            const Index a = tris[uz(f)][uz(k)], b = tris[uz(f)][uz((k + 1) % 3)];
            const auto it = byDir.find({b, a});  // opposite orientation
            if (it != byDir.end()) {
                e2e[uz(f) * 3 + uz(k)] = it->second;
            }
        }
    }
    return e2e;
}

}  // namespace

McfEdgeInfo buildMcfEdgeInfo(const Mesh& mesh, const PositionField& field) {
    McfEdgeInfo info;
    const double scale = field.spacing > 0.0f ? static_cast<double>(field.spacing) : 1.0;
    const double invS = 1.0 / scale;

    // Compact list of alive triangles + their vertex ids.
    std::vector<std::array<Index, 3>> tris;
    for (Index i = 0; i < mesh.faceCapacity(); ++i) {
        const FaceId f{i};
        if (!mesh.isAlive(f) || mesh.faceSize(f) != 3) {
            continue;
        }
        const std::vector<VertexId> fv = mesh.faceVertices(f);
        tris.push_back({fv[0].value, fv[1].value, fv[2].value});
        info.faceVerts.push_back({fv[0].value, fv[1].value, fv[2].value});
        info.faceList.push_back(i);
    }
    const int m = static_cast<int>(tris.size());
    if (m == 0) {
        return info;
    }

    // Mutable orientation copy (ComputeOrientationSingularities flips some entries).
    auto q = [&](Index v) { return normalize(d3(field.q[v])); };
    std::vector<D3> Q(mesh.vertexCapacity());
    std::vector<D3> Nn(mesh.vertexCapacity());
    for (int f = 0; f < m; ++f) {
        for (int k = 0; k < 3; ++k) {
            const Index v = tris[uz(f)][uz(k)];
            Q[v] = q(v);
            Nn[v] = d3(field.normal[v]);
        }
    }

    // 1. Orientation singularities (parametrizer-sing.cpp). May flip Q[F(0,f)].
    for (int f = 0; f < m; ++f) {
        int index = 0;
        for (int k = 0; k < 3; ++k) {
            const Index i = tris[uz(f)][uz(k)], j = tris[uz(f)][uz((k + 1) % 3)];
            const auto val = orientIndex4(Q[i], Nn[i], Q[j], Nn[j]);
            index += val.second - val.first;
        }
        const int im = modulo(index, 4);
        if (im == 1 || im == 3) {
            if (index >= 4 || index < 0) {
                const Index v0 = tris[uz(f)][0];
                Q[v0] = Q[v0] * -1.0;
            }
            info.orientSingularities[f] = im;
        }
    }

    // 2. Position singularities + pos_rank / pos_index (parametrizer-sing.cpp).
    std::vector<std::array<int, 3>> posRank(uz(m));
    std::vector<std::array<int, 6>> posIndex(uz(m));
    for (int f = 0; f < m; ++f) {
        const Index id[3] = {tris[uz(f)][0], tris[uz(f)][1], tris[uz(f)][2]};
        D3 qf[3] = {Q[id[0]], Q[id[1]], Q[id[2]]};
        const D3 nf[3] = {Nn[id[0]], Nn[id[1]], Nn[id[2]]};
        const D3 of[3] = {d3(field.o[id[0]]), d3(field.o[id[1]]), d3(field.o[id[2]])};
        const D3 vf[3] = {d3(mesh.position(VertexId{id[0]})), d3(mesh.position(VertexId{id[1]})),
                          d3(mesh.position(VertexId{id[2]}))};

        int best[3] = {0, 0, 0};
        double bestDp = -std::numeric_limits<double>::infinity();
        for (int i = 0; i < 4; ++i) {
            const D3 v0 = rotate90by(qf[0], nf[0], i);
            for (int j = 0; j < 4; ++j) {
                const D3 v1 = rotate90by(qf[1], nf[1], j);
                for (int k = 0; k < 4; ++k) {
                    const D3 v2 = rotate90by(qf[2], nf[2], k);
                    const double dp = std::min({dot(v0, v1), dot(v1, v2), dot(v2, v0)});
                    if (dp > bestDp) {
                        bestDp = dp;
                        best[0] = i;
                        best[1] = j;
                        best[2] = k;
                    }
                }
            }
        }
        posRank[uz(f)] = {best[0], best[1], best[2]};
        for (int k = 0; k < 3; ++k) {
            qf[k] = rotate90by(qf[k], nf[k], best[k]);
        }
        Vec2i index{0, 0};
        for (int k = 0; k < 3; ++k) {
            const int kn = (k + 1) % 3;
            const auto val = positionIndex4(vf[k], nf[k], qf[k], of[k], vf[kn], nf[kn], qf[kn],
                                            of[kn], scale, scale, invS, invS);
            const Vec2i diff{val.first.x - val.second.x, val.first.y - val.second.y};
            index = {index.x + diff.x, index.y + diff.y};
            posIndex[uz(f)][uz(k * 2)] = diff.x;
            posIndex[uz(f)][uz(k * 2 + 1)] = diff.y;
        }
        if (index.x != 0 || index.y != 0) {
            info.posSingularities[f] = rshift90(index, best[0]);
        }
    }

    // 3. BuildEdgeInfo (parametrizer-int.cpp): edge_diff / edge_values / faceEdgeIds.
    const auto sz = uz;
    const std::vector<int> e2e = buildE2E(tris);
    info.faceEdgeIds.assign(sz(m), {-1, -1, -1});
    for (int f = 0; f < m; ++f) {
        for (int k1 = 0; k1 < 3; ++k1) {
            const int k2 = (k1 + 1) % 3;
            const Index v1 = tris[sz(f)][sz(k1)], v2 = tris[sz(f)][sz(k2)];
            Vec2i diff2;
            if (v1 > v2) {
                const int rank2 = posRank[sz(f)][sz(k2)];
                diff2 = rshift90({-posIndex[sz(f)][sz(k1 * 2)], -posIndex[sz(f)][sz(k1 * 2 + 1)]},
                                 rank2);
            } else {
                const int rank2 = posRank[sz(f)][sz(k1)];
                diff2 =
                    rshift90({posIndex[sz(f)][sz(k1 * 2)], posIndex[sz(f)][sz(k1 * 2 + 1)]}, rank2);
            }
            const int currentEid = f * 3 + k1;
            const int eid = e2e[sz(currentEid)];
            const int eID1 = info.faceEdgeIds[sz(f)][sz(k1)];
            if (eID1 == -1) {
                const int eID2 = static_cast<int>(info.edgeValues.size());
                info.edgeValues.emplace_back(std::min(v1, v2), std::max(v1, v2));
                info.edgeDiff.push_back(diff2);
                info.faceEdgeIds[sz(f)][sz(k1)] = eID2;
                if (eid != -1) {
                    info.faceEdgeIds[sz(eid / 3)][sz(eid % 3)] = eID2;
                }
            } else if (info.orientSingularities.find(f) == info.orientSingularities.end()) {
                const int eID2 = info.faceEdgeIds[sz(eid / 3)][sz(eid % 3)];
                info.edgeDiff[sz(eID2)] = diff2;
            }
        }
    }

    // Expose the post-flip orientation (Q, mutated by step 1) so the M3 constraint
    // builder reads the same field the singularity/edge_diff computation used.
    info.orient.assign(mesh.vertexCapacity(), Vec3{0, 0, 0});
    for (int f = 0; f < m; ++f) {
        for (int k = 0; k < 3; ++k) {
            const Index v = tris[uz(f)][uz(k)];
            info.orient[v] = Vec3{static_cast<float>(Q[v].x), static_cast<float>(Q[v].y),
                                  static_cast<float>(Q[v].z)};
        }
    }

    info.valid = true;
    return info;
}

}  // namespace cyber::remesh
