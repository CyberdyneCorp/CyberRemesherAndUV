// Bi-MDF quantization: motorcycle-graph / T-mesh extraction from the relaxed
// seamless map, and the S1 min-deviation-flow solve. See bimdf_quantize.hpp.
#include "bimdf_quantize.hpp"

#include <algorithm>
#include <cmath>
#include <cstdio>
#include <cstdlib>
#include <functional>
#include <limits>
#include <map>
#include <optional>
#include <string>
#include <tuple>
#include <unordered_set>

#include "min_cost_flow.hpp"

namespace cyber::remesh::bimdf {
namespace {

constexpr std::size_t kNone = std::numeric_limits<std::size_t>::max();
constexpr double kPiD = 3.14159265358979323846;

// ---------------------------------------------------------------------------
// Small numeric helpers. Everything below works in grid-cell UV units.
// ---------------------------------------------------------------------------
struct V2 {
    double x = 0.0, y = 0.0;
};
inline double comp(const V2& p, int axis) { return axis == 0 ? p.x : p.y; }
inline void setComp(V2& p, int axis, double v) { (axis == 0 ? p.x : p.y) = v; }

// Integer 2x2 rotation matrix R^rho, R = CCW (u,v) -> (-v,u).
struct RotM {
    int m[2][2] = {{1, 0}, {0, 1}};
};
RotM rotPow(int rho) {
    switch (((rho % 4) + 4) % 4) {
        case 0:
            return RotM{{{1, 0}, {0, 1}}};
        case 1:
            return RotM{{{0, -1}, {1, 0}}};
        case 2:
            return RotM{{{-1, 0}, {0, -1}}};
        default:
            return RotM{{{0, 1}, {-1, 0}}};
    }
}
V2 apply(const RotM& r, const V2& p) {
    return V2{r.m[0][0] * p.x + r.m[0][1] * p.y, r.m[1][0] * p.x + r.m[1][1] * p.y};
}
RotM mul(const RotM& a, const RotM& b) {
    RotM c;
    for (int i = 0; i < 2; ++i) {
        for (int j = 0; j < 2; ++j) {
            c.m[i][j] = a.m[i][0] * b.m[0][j] + a.m[i][1] * b.m[1][j];
        }
    }
    return c;
}
RotM invRot(const RotM& r) {
    RotM t;
    t.m[0][0] = r.m[0][0];
    t.m[0][1] = r.m[1][0];
    t.m[1][0] = r.m[0][1];
    t.m[1][1] = r.m[1][1];
    return t;  // rotations: inverse == transpose
}
// Quarter direction: 0=+u, 1=+v, 2=-u, 3=-v.
V2 quarterDir(int q) {
    switch (((q % 4) + 4) % 4) {
        case 0:
            return V2{1.0, 0.0};
        case 1:
            return V2{0.0, 1.0};
        case 2:
            return V2{-1.0, 0.0};
        default:
            return V2{0.0, -1.0};
    }
}
int rotateQuarter(const RotM& r, int q) {
    const V2 d = apply(r, quarterDir(q));
    if (d.x > 0.5) {
        return 0;
    }
    if (d.y > 0.5) {
        return 1;
    }
    if (d.x < -0.5) {
        return 2;
    }
    return 3;
}
inline int varyAxis(int q) { return (q % 2 == 0) ? 0 : 1; }
inline int constAxis(int q) { return (q % 2 == 0) ? 1 : 0; }
inline int travelSign(int q) { return q < 2 ? 1 : -1; }

// ---------------------------------------------------------------------------
// ZExpr helpers.
// ---------------------------------------------------------------------------
void addScaled(ZExpr& dst, const ZExpr& src, double k) {
    if (k == 0.0) {
        return;
    }
    for (const auto& [i, w] : src) {
        dst[i] += k * w;
    }
}
void addVar(ZExpr& dst, std::size_t i, double k) { dst[i] += k; }
void pruneExpr(ZExpr& e) {
    for (auto it = e.begin(); it != e.end();) {
        if (std::abs(it->second) < 1e-9) {
            it = e.erase(it);
        } else {
            ++it;
        }
    }
}
double evalExpr(const ZExpr& e, const std::vector<double>& z) {
    double s = 0.0;
    for (const auto& [i, w] : e) {
        s += w * z[i];
    }
    return s;
}
ZExpr unitExpr(std::size_t var) {
    ZExpr e;
    e[var] = 1.0;
    return e;
}
ZExpr diffExpr(const ZExpr& a, const ZExpr& b, double sign) {
    ZExpr r = a;
    addScaled(r, b, -1.0);
    if (sign != 1.0) {
        for (auto& [i, w] : r) {
            w *= sign;
        }
    }
    pruneExpr(r);
    return r;
}

// ---------------------------------------------------------------------------
// T-mesh builder.
// ---------------------------------------------------------------------------
class Builder {
public:
    Builder(const Charts& ch, const std::vector<double>& z) : ch_(ch), z_(z) {}

    TMesh run() {
        TMesh tm;
        for (std::size_t c = 0; c < ch_.nCut; ++c) {
            if (ch_.coneIndex[c] != 0) {
                coneOfVertex_.emplace(ch_.vertexOfCut[c], ch_.coneIndex[c]);
            }
        }
        tm.coneNodes = coneOfVertex_.size();
        if (!buildAdjacency(tm)) {
            return tm;
        }
        buildCreaseChains(tm);
        if (!traceRays(tm)) {
            tm.failedRays = failedLaunches_;
            return tm;
        }
        assembleArcs(tm);
        if (!tm.reason.empty()) {
            return tm;
        }
        const bool patchesOk = buildPatches(tm);
        tm.failedRays = failedLaunches_;
        tm.degradedNodes = degradedNodes_;
        tm.repairedNodes = repairedNodes_;
        if (!patchesOk) {
            return tm;
        }
        if (failedLaunches_ != 0) {
            // Arcs and quad patches around the abandoned launches are
            // structurally suspect even when every orbit closed with 4
            // corners; be honest and refuse the T-mesh.
            tm.reason = "abandoned separatrix launches: " + std::to_string(failedLaunches_);
            return tm;
        }
        tm.ok = true;
        return tm;
    }

private:
    // ---- geometry access -------------------------------------------------
    V2 uv(std::size_t cut) const {
        return V2{static_cast<double>(ch_.u[cut]), static_cast<double>(ch_.v[cut])};
    }

    // ---- adjacency -------------------------------------------------------
    struct Nb {
        std::size_t face = kNone;  // neighbor face (kNone: boundary)
        int seam = -1;             // seam index (-1: same chart)
        bool aToB = true;          // crossing direction when seam >= 0
    };

    static std::uint64_t pairKey(std::size_t a, std::size_t b) {
        const std::uint64_t lo = std::min(a, b), hi = std::max(a, b);
        return (hi << 32) | lo;
    }

    bool buildAdjacency(TMesh& tm) {
        const auto& faces = ch_.faces;
        nbs_.assign(faces.size(), {});
        seamOfFaceEdge_.assign(faces.size(), {-1, -1, -1});
        std::unordered_map<std::uint64_t, std::vector<std::pair<std::size_t, int>>> byPair;
        for (std::size_t f = 0; f < faces.size(); ++f) {
            for (int e = 0; e < 3; ++e) {
                const std::size_t a = faces[f][static_cast<std::size_t>(e)];
                const std::size_t b = faces[f][static_cast<std::size_t>((e + 1) % 3)];
                byPair[pairKey(a, b)].push_back({f, e});
            }
        }
        const auto findLocal = [&](std::size_t f, std::size_t a, std::size_t b) -> int {
            for (int e = 0; e < 3; ++e) {
                const std::size_t x = ch_.faces[f][static_cast<std::size_t>(e)];
                const std::size_t y = ch_.faces[f][static_cast<std::size_t>((e + 1) % 3)];
                if ((x == a && y == b) || (x == b && y == a)) {
                    return e;
                }
            }
            return -1;
        };
        for (std::size_t s = 0; s < ch_.seams.size(); ++s) {
            const SeamEdge& se = ch_.seams[s];
            const int eA = findLocal(se.faceA, se.aA, se.bA);
            const int eB = findLocal(se.faceB, se.aB, se.bB);
            if (eA < 0 || eB < 0) {
                tm.reason = "seam edge not found in incident face";
                return false;
            }
            nbs_[se.faceA][static_cast<std::size_t>(eA)] = {se.faceB, static_cast<int>(s), true};
            nbs_[se.faceB][static_cast<std::size_t>(eB)] = {se.faceA, static_cast<int>(s), false};
            seamOfFaceEdge_[se.faceA][static_cast<std::size_t>(eA)] = static_cast<int>(s);
            seamOfFaceEdge_[se.faceB][static_cast<std::size_t>(eB)] = static_cast<int>(s);
        }
        for (std::size_t f = 0; f < faces.size(); ++f) {
            for (int e = 0; e < 3; ++e) {
                if (nbs_[f][static_cast<std::size_t>(e)].face != kNone ||
                    seamOfFaceEdge_[f][static_cast<std::size_t>(e)] >= 0) {
                    continue;
                }
                const std::size_t a = faces[f][static_cast<std::size_t>(e)];
                const std::size_t b = faces[f][static_cast<std::size_t>((e + 1) % 3)];
                for (const auto& [g, ge] : byPair[pairKey(a, b)]) {
                    if (g != f) {
                        nbs_[f][static_cast<std::size_t>(e)] = {g, -1, true};
                    }
                }
                if (nbs_[f][static_cast<std::size_t>(e)].face == kNone) {
                    // Crack / non-manifold sliver in the work mesh: the solve
                    // itself treats it as free boundary. Tolerated as a wall;
                    // the build only fails if a ray or a node fan reaches it.
                    ++openEdges_;
                }
            }
        }
        if (std::getenv("CYBER_QC_BIMDF_DEBUG") != nullptr) {
            // Invariant check: the relaxed UVs satisfy every seam constraint
            // uv_B = R^rho uv_A + t with one t per edge (evaluated at both
            // endpoints). Large residuals mean the transition semantics are
            // not what the tracer assumes.
            double worst = 0.0;
            std::size_t bad = 0;
            for (const SeamEdge& se : ch_.seams) {
                const RotM r = rotPow(se.rho);
                const V2 ta = {uv(se.aB).x - apply(r, uv(se.aA)).x,
                               uv(se.aB).y - apply(r, uv(se.aA)).y};
                const V2 tb = {uv(se.bB).x - apply(r, uv(se.bA)).x,
                               uv(se.bB).y - apply(r, uv(se.bA)).y};
                const double d = std::max(std::abs(ta.x - tb.x), std::abs(ta.y - tb.y));
                worst = std::max(worst, d);
                if (d > 1e-3) {
                    ++bad;
                }
            }
            std::fprintf(stderr, "[qc] bimdf seam invariant: worst=%.3e bad=%zu of %zu open=%zu\n",
                         worst, bad, ch_.seams.size(), openEdges_);
        }
        (void)tm;
        return true;
    }

    // Transition across seam s in the given direction: X' = R X + t.
    struct Trans {
        RotM R;
        V2 tN;
        std::array<ZExpr, 2> tS;
    };
    Trans transition(int s, bool aToB) const {
        const SeamEdge& se = ch_.seams[static_cast<std::size_t>(s)];
        Trans tr;
        if (aToB) {
            tr.R = rotPow(se.rho);
            const V2 ra = apply(tr.R, uv(se.aA));
            tr.tN = V2{uv(se.aB).x - ra.x, uv(se.aB).y - ra.y};
            addVar(tr.tS[0], se.tx, 1.0);
            addVar(tr.tS[1], se.ty, 1.0);
        } else {
            tr.R = rotPow(-se.rho);
            const V2 rt = apply(tr.R, uv(se.aB));
            tr.tN = V2{uv(se.aA).x - rt.x, uv(se.aA).y - rt.y};
            // t' = -R^{-rho} (tx, ty).
            for (int i = 0; i < 2; ++i) {
                for (int j = 0; j < 2; ++j) {
                    const int k = tr.R.m[i][j];
                    if (k != 0) {
                        addVar(tr.tS[static_cast<std::size_t>(i)], j == 0 ? se.tx : se.ty,
                               -static_cast<double>(k));
                    }
                }
            }
        }
        return tr;
    }

    std::size_t crossCut(int s, bool aToB, std::size_t cut) const {
        const SeamEdge& se = ch_.seams[static_cast<std::size_t>(s)];
        if (aToB) {
            if (cut == se.aA) {
                return se.aB;
            }
            if (cut == se.bA) {
                return se.bB;
            }
        } else {
            if (cut == se.aB) {
                return se.aA;
            }
            if (cut == se.bB) {
                return se.bA;
            }
        }
        return kNone;
    }

    // ---- nodes -----------------------------------------------------------
    struct Node {
        int type = 0;  // 0 = mesh vertex, 1 = interior point (T-node)
        std::uint32_t meshVertex = 0;
    };
    std::size_t vertexNode(std::uint32_t v) {
        const auto it = vertexNode_.find(v);
        if (it != vertexNode_.end()) {
            return it->second;
        }
        const std::size_t id = nodes_.size();
        nodes_.push_back(Node{0, v});
        vertexNode_.emplace(v, id);
        return id;
    }
    std::size_t interiorNode() {
        const std::size_t id = nodes_.size();
        nodes_.push_back(Node{1, 0});
        return id;
    }
    bool isConeVertex(std::uint32_t v) const { return coneOfVertex_.count(v) != 0; }

    // ---- crease chains ---------------------------------------------------
    struct ChainEdge {
        int seam;      // seam index
        bool forward;  // walk enters at aA (true) or bA (false)
    };
    struct ChainEvent {
        double along = 0.0;
        ZExpr expr;            // accumulated along-length expression at this point
        std::size_t node = 0;  // node id
        std::size_t edge = 0;  // chain edge index under the event
        double frac = 0.0;     // position inside that edge (0 = edge start)
    };
    struct Chain {
        std::vector<ChainEdge> edges;
        std::vector<double> accum;       // accumulated length at edge starts (size edges+1)
        std::vector<ZExpr> accumExpr;    // symbolic accumulated length
        std::vector<std::uint32_t> vAt;  // mesh vertex at each edge start/end
        std::vector<ChainEvent> events;
        bool closed = false;
    };

    double chainCoord(const SeamEdge& se, std::size_t cut) const {
        return comp(uv(cut), 1 - se.pinAxis);
    }
    std::size_t chainCoordVar(const SeamEdge& se, std::size_t cut) const {
        return se.pinAxis == 0 ? ch_.vIx(cut) : ch_.uIx(cut);
    }

    void buildCreaseChains(TMesh& tm) {
        const std::size_t nSeam = ch_.seams.size();
        std::vector<char> used(nSeam, 0);
        std::unordered_map<std::uint32_t, std::vector<int>> at;
        for (std::size_t s = 0; s < nSeam; ++s) {
            if (!ch_.seams[s].feature) {
                continue;
            }
            at[ch_.vertexOfCut[ch_.seams[s].aA]].push_back(static_cast<int>(s));
            at[ch_.vertexOfCut[ch_.seams[s].bA]].push_back(static_cast<int>(s));
        }
        const auto isJunction = [&](std::uint32_t v) {
            const auto it = at.find(v);
            const std::size_t deg = it == at.end() ? 0 : it->second.size();
            return deg != 2 || isConeVertex(v);
        };
        const auto walkFrom = [&](int s0, std::uint32_t vStart) {
            Chain c;
            int s = s0;
            std::uint32_t v = vStart;
            c.vAt.push_back(v);
            while (true) {
                const SeamEdge& se = ch_.seams[static_cast<std::size_t>(s)];
                used[static_cast<std::size_t>(s)] = 1;
                const bool fwd = ch_.vertexOfCut[se.aA] == v;
                c.edges.push_back({s, fwd});
                v = fwd ? ch_.vertexOfCut[se.bA] : ch_.vertexOfCut[se.aA];
                c.vAt.push_back(v);
                if (isJunction(v) || v == vStart) {
                    break;
                }
                int next = -1;
                for (const int cand : at[v]) {
                    if (!used[static_cast<std::size_t>(cand)]) {
                        next = cand;
                        break;
                    }
                }
                if (next < 0) {
                    break;
                }
                s = next;
            }
            // Accumulated lengths at each edge start.
            c.accum.push_back(0.0);
            c.accumExpr.push_back({});
            for (const ChainEdge& ce : c.edges) {
                const SeamEdge& se = ch_.seams[static_cast<std::size_t>(ce.seam)];
                const std::size_t c0 = ce.forward ? se.aA : se.bA;
                const std::size_t c1 = ce.forward ? se.bA : se.aA;
                const double d = chainCoord(se, c1) - chainCoord(se, c0);
                const double sg = d >= 0.0 ? 1.0 : -1.0;
                c.accum.push_back(c.accum.back() + sg * d);
                ZExpr e = c.accumExpr.back();
                addVar(e, chainCoordVar(se, c1), sg);
                addVar(e, chainCoordVar(se, c0), -sg);
                pruneExpr(e);
                c.accumExpr.push_back(std::move(e));
            }
            c.closed = c.vAt.back() == vStart && !isJunction(vStart);
            chains_.push_back(std::move(c));
        };
        for (std::size_t s = 0; s < nSeam; ++s) {
            if (!ch_.seams[s].feature || used[s]) {
                continue;
            }
            const std::uint32_t va = ch_.vertexOfCut[ch_.seams[s].aA];
            const std::uint32_t vb = ch_.vertexOfCut[ch_.seams[s].bA];
            if (isJunction(va)) {
                walkFrom(static_cast<int>(s), va);
            } else if (isJunction(vb)) {
                walkFrom(static_cast<int>(s), vb);
            }
        }
        for (std::size_t s = 0; s < nSeam; ++s) {
            if (ch_.seams[s].feature && !used[s]) {
                walkFrom(static_cast<int>(s), ch_.vertexOfCut[ch_.seams[s].aA]);
            }
        }
        // Prune STUB chains: a crease line dead-ending at a non-cone vertex is
        // not a T-mesh arc (it only pins the local grid phase, which stays
        // with the reduced integer solve); rays cross it like any other seam.
        // Iterate: pruning can expose new dead ends.
        std::vector<char> dead(chains_.size(), 0);
        bool changed = true;
        while (changed) {
            changed = false;
            std::unordered_map<std::uint32_t, int> deg;
            for (std::size_t c = 0; c < chains_.size(); ++c) {
                if (dead[c] || chains_[c].closed) {
                    continue;
                }
                ++deg[chains_[c].vAt.front()];
                ++deg[chains_[c].vAt.back()];
            }
            for (std::size_t c = 0; c < chains_.size(); ++c) {
                if (dead[c] || chains_[c].closed) {
                    continue;
                }
                for (const std::uint32_t v : {chains_[c].vAt.front(), chains_[c].vAt.back()}) {
                    if (!isConeVertex(v) && deg[v] == 1) {
                        dead[c] = 1;
                        changed = true;
                        break;
                    }
                }
            }
        }
        std::vector<Chain> kept;
        for (std::size_t c = 0; c < chains_.size(); ++c) {
            if (!dead[c]) {
                kept.push_back(std::move(chains_[c]));
            } else {
                ++tm.prunedChains;
            }
        }
        chains_ = std::move(kept);
        // Endpoint nodes, seam registration, interior-vertex registry.
        for (std::size_t chainId = 0; chainId < chains_.size(); ++chainId) {
            Chain& c = chains_[chainId];
            for (std::size_t i = 0; i < c.edges.size(); ++i) {
                chainEdgeIndex_.emplace(c.edges[i].seam, std::make_pair(chainId, i));
                creaseArcSeam_.insert(c.edges[i].seam);
            }
            if (c.closed) {
                creaseEndCount_[c.vAt.front()] += 2;
            } else {
                ++creaseEndCount_[c.vAt.front()];
                ++creaseEndCount_[c.vAt.back()];
            }
            ChainEvent e0;
            e0.along = 0.0;
            e0.node = vertexNode(c.vAt.front());
            e0.edge = 0;
            e0.frac = 0.0;
            c.events.push_back(e0);
            ChainEvent e1;
            e1.along = c.accum.back();
            e1.expr = c.accumExpr.back();
            e1.node = c.closed ? c.events[0].node : vertexNode(c.vAt.back());
            e1.edge = c.edges.size() - 1;
            e1.frac = 1.0;
            c.events.push_back(e1);
            for (std::size_t k = 1; k + 1 < c.vAt.size(); ++k) {
                chainPosOfVertex_.emplace(c.vAt[k],
                                          std::make_tuple(chainId, c.accum[k], c.accumExpr[k], k));
            }
        }
        tm.creaseNodes = vertexNode_.size();
    }

    // ---- ray tracing -----------------------------------------------------
    // The walk is contour-following on the piecewise-linear level set
    // {coord == c}: enter a face through one edge, leave through the unique
    // other edge the level crosses. This is robust to UV foldovers (the
    // relaxed map is not injective near high-distortion cones), where a
    // direction-based march would try to walk "forward" out of a face whose
    // image lies behind the entry edge. Two parameters ride along:
    //   curve — arc length of the walk itself (monotone; orders events);
    //   along — the chart0 along-coordinate (signed; may locally reverse in
    //           folded regions; differences give the arc lengths and match
    //           the symbolic expressions exactly).
    struct Seg {
        int ray;
        int gen;            // owner generation (stale after a jittered retry)
        int lc;             // constant axis in this face chart
        double c;           // level value in this chart
        ZExpr cExpr;        // level expression in this chart
        double a0, a1;      // varying-coordinate range (a0 < a1)
        double pvSeg;       // varying coordinate at the owner's entry point
        double alongEntry;  // owner's chart0 along-coordinate at the entry point
        double curveEntry;  // owner's curve parameter at the entry point
        int alongSlope;     // d(along)/d(varying coordinate) = +-1
        RotM Rm0;           // owner chart0 recovery: R^{-Q}
        V2 T;
        std::array<ZExpr, 2> Tsym;
        int va0;
        int s0;
    };
    struct Event {
        double along = 0.0;
        double curve = 0.0;  // walk parameter (sort key)
        ZExpr expr;          // chart0 along-coordinate expression (unsigned)
        std::size_t node = 0;
        std::size_t face = 0;        // anchor chart
        int q = 0;                   // owner travel quarter at the event, anchor chart
        std::size_t corner = kNone;  // anchor cut vertex for vertex-fan nodes
    };
    struct Ray {
        std::size_t startNode = 0;
        std::size_t startCut = 0;
        std::size_t startFace = 0;
        int q0 = 0;
        double startAlong = 0.0;
        ZExpr startExpr;
        std::vector<Event> events;  // T-nodes on this ray + termination
        bool failed = false;        // launch exhausted at a folded cone; no arcs
    };

    struct WalkState {
        std::size_t face = 0;
        V2 p;
        int q = 0;
        double c = 0.0;  // constant-axis level, current chart
        ZExpr cExpr;
        RotM RQ;  // chart0 -> current chart: X_k = R^Q X_0 + T
        V2 T;
        std::array<ZExpr, 2> Tsym;
        int va0 = 0;
        int s0 = 1;
        int enteredEdge = -1;
        double curve = 0.0;  // walk arc length so far (monotone)
    };

    double alongOf(const WalkState& w) const {
        const RotM ri = invRot(w.RQ);
        const V2 x0 = apply(ri, V2{w.p.x - w.T.x, w.p.y - w.T.y});
        return w.s0 * comp(x0, w.va0);
    }
    // d(along)/d(varying coordinate) in the current chart: +-1.
    int alongSlopeOf(const WalkState& w) const {
        const RotM ri = invRot(w.RQ);
        return w.s0 * ri.m[w.va0][varyAxis(w.q)];
    }
    // chart0 along-expression of a point whose current-chart coordinate
    // expressions are known: constant axis -> w.cExpr, varying axis -> aE.
    ZExpr along0Expr(const WalkState& w, const ZExpr& aE) const {
        const RotM ri = invRot(w.RQ);
        const ZExpr* comps[2];
        ZExpr cc = w.cExpr;
        ZExpr ac = aE;
        comps[constAxis(w.q)] = &cc;
        comps[varyAxis(w.q)] = &ac;
        ZExpr out;
        for (int j = 0; j < 2; ++j) {
            const double k = static_cast<double>(ri.m[w.va0][j]);
            if (k == 0.0) {
                continue;
            }
            addScaled(out, *comps[j], k);
            addScaled(out, w.Tsym[static_cast<std::size_t>(j)], -k);
        }
        pruneExpr(out);
        return out;
    }

    void applyTransition(WalkState& w, const Trans& tr) const {
        w.p = apply(tr.R, w.p);
        w.p.x += tr.tN.x;
        w.p.y += tr.tN.y;
        const int oldLc = constAxis(w.q);
        w.q = rotateQuarter(tr.R, w.q);
        const int newLc = constAxis(w.q);
        const double k = static_cast<double>(tr.R.m[newLc][oldLc]);
        ZExpr ne;
        addScaled(ne, w.cExpr, k);
        addScaled(ne, tr.tS[static_cast<std::size_t>(newLc)], 1.0);
        pruneExpr(ne);
        w.cExpr = std::move(ne);
        w.c = k * w.c + comp(tr.tN, newLc);
        w.RQ = mul(tr.R, w.RQ);
        const V2 rt = apply(tr.R, w.T);
        w.T = V2{rt.x + tr.tN.x, rt.y + tr.tN.y};
        std::array<ZExpr, 2> nt;
        for (int i = 0; i < 2; ++i) {
            for (int j = 0; j < 2; ++j) {
                const double kk = static_cast<double>(tr.R.m[i][j]);
                if (kk != 0.0) {
                    addScaled(nt[static_cast<std::size_t>(i)], w.Tsym[static_cast<std::size_t>(j)],
                              kk);
                }
            }
            addScaled(nt[static_cast<std::size_t>(i)], tr.tS[static_cast<std::size_t>(i)], 1.0);
            pruneExpr(nt[static_cast<std::size_t>(i)]);
        }
        w.Tsym = std::move(nt);
    }

    // A live motorcycle: the walk front plus the ray record under
    // construction. Rays advance in LOCKSTEP (always the smallest curve
    // parameter next), the standard motorcycle-graph semantics — a
    // sequentially traced ray would have nothing to crash into.
    struct LaunchOpt {
        std::size_t face = 0, cut = 0;
        int q = 0;
        std::array<double, 3> d3{};
    };
    struct Walk {
        WalkState w;
        int id = 0;
        int attempt = 0;
        std::size_t steps = 0;
        bool done = false;
        std::size_t launchFace = 0, launchCut = 0;
        int launchQ = 0;
        std::array<double, 3> d3{};   // intrinsic launch direction (3D)
        std::vector<LaunchOpt> alts;  // same-class fallback launch sites
        std::size_t altIdx = 0;
    };

    void initWalk(Walk& wk) const {
        const double jitter = wk.attempt == 0 ? 0.0
                                              : (wk.attempt % 2 == 1 ? 1.0 : -1.0) * 3e-4 *
                                                    static_cast<double>(wk.attempt);
        WalkState w;
        w.face = wk.launchFace;
        w.p = uv(wk.launchCut);
        w.q = wk.launchQ;
        const int lc0 = constAxis(wk.launchQ);
        w.c = comp(w.p, lc0) + jitter;
        setComp(w.p, lc0, w.c);
        addVar(w.cExpr, lc0 == 0 ? ch_.uIx(wk.launchCut) : ch_.vIx(wk.launchCut), 1.0);
        w.va0 = varyAxis(wk.launchQ);
        w.s0 = travelSign(wk.launchQ);
        wk.w = std::move(w);
        wk.steps = 0;
    }

    bool traceRays(TMesh& tm) {
        faceSegs_.assign(ch_.faces.size(), {});
        // Canonical key for a mesh edge (seam-side pairs collapse onto side A),
        // used to dedupe on-boundary launch claims across seam sides.
        std::unordered_map<std::uint64_t, std::uint64_t> canonEdge;
        for (const SeamEdge& se : ch_.seams) {
            canonEdge.emplace(pairKey(se.aB, se.bB), pairKey(se.aA, se.bA));
        }
        const auto canon = [&](std::size_t a, std::size_t b) {
            const std::uint64_t k = pairKey(a, b);
            const auto it = canonEdge.find(k);
            return it == canonEdge.end() ? k : it->second;
        };
        // Collect launch candidates per cone: a cone of index s owns exactly
        // 4 - s separatrix directions; per-wedge inside tests lose some of
        // them on folded relaxed maps, so candidates are ranked by their
        // wedge margin and the best distinct 4 - s are launched.
        struct Cand {
            std::size_t face, cut;
            int q;
            double margin;
            std::uint64_t edgeKey;  // dedupe key for on-boundary claims (0: none)
            bool uvOk;              // launch level exits this face's UV wedge
        };
        // The wedge-inside test runs in 3D (mesh positions + combed frame):
        // the relaxed UV can fold near high-distortion cones, but the
        // intrinsic wedge geometry is always clean. Chart quarter q maps to
        // the 3D direction R_n(q * 90deg) e0 by construction of the charts.
        const auto v3 = [](const std::array<float, 3>& a) {
            return std::array<double, 3>{static_cast<double>(a[0]), static_cast<double>(a[1]),
                                         static_cast<double>(a[2])};
        };
        const auto sub3 = [](const std::array<double, 3>& a, const std::array<double, 3>& b) {
            return std::array<double, 3>{a[0] - b[0], a[1] - b[1], a[2] - b[2]};
        };
        const auto cross3 = [](const std::array<double, 3>& a, const std::array<double, 3>& b) {
            return std::array<double, 3>{a[1] * b[2] - a[2] * b[1], a[2] * b[0] - a[0] * b[2],
                                         a[0] * b[1] - a[1] * b[0]};
        };
        const auto dot3 = [](const std::array<double, 3>& a, const std::array<double, 3>& b) {
            return a[0] * b[0] + a[1] * b[1] + a[2] * b[2];
        };
        const auto norm3 = [&](const std::array<double, 3>& a) { return std::sqrt(dot3(a, a)); };
        // Fan face order + accumulated chart rotation around a vertex
        // (combinatorial; drives class-pattern launch selection).
        const auto fanOrder =
            [&](std::uint32_t v, std::size_t f0, std::size_t c0,
                std::unordered_map<std::size_t, std::pair<std::size_t, int>>& order,
                int* holQcum = nullptr) {
                std::size_t f = f0, cornerCut = c0;
                int qcum = 0;
                std::size_t guard = 0;
                while (guard++ <= ch_.faces.size()) {
                    order.emplace(f, std::make_pair(order.size(), qcum));
                    int corner = -1;
                    for (int k = 0; k < 3; ++k) {
                        if (ch_.faces[f][static_cast<std::size_t>(k)] == cornerCut) {
                            corner = k;
                        }
                    }
                    if (corner < 0) {
                        return;
                    }
                    const Nb& nbr = nbs_[f][static_cast<std::size_t>((corner + 2) % 3)];
                    if (nbr.face == kNone) {
                        return;
                    }
                    std::size_t nextCorner = cornerCut;
                    if (nbr.seam >= 0) {
                        nextCorner = crossCut(nbr.seam, nbr.aToB, cornerCut);
                        if (nextCorner == kNone) {
                            return;
                        }
                        const int rho = ch_.seams[static_cast<std::size_t>(nbr.seam)].rho;
                        qcum += nbr.aToB ? rho : -rho;
                    }
                    if (nbr.face == f0 && nextCorner == c0) {
                        if (holQcum != nullptr) {
                            *holQcum = qcum;  // closed fan: seam holonomy known
                        }
                        return;
                    }
                    f = nbr.face;
                    cornerCut = nextCorner;
                }
                (void)v;
            };
        std::unordered_map<std::uint32_t, std::vector<Cand>> byCone;
        for (std::size_t f = 0; f < ch_.faces.size(); ++f) {
            for (int k = 0; k < 3; ++k) {
                const std::size_t cut = ch_.faces[f][static_cast<std::size_t>(k)];
                if (ch_.coneIndex[cut] == 0) {
                    continue;
                }
                const std::size_t nxt = ch_.faces[f][static_cast<std::size_t>((k + 1) % 3)];
                const std::size_t prv = ch_.faces[f][static_cast<std::size_t>((k + 2) % 3)];
                const auto pc = v3(ch_.vertexPos[ch_.vertexOfCut[cut]]);
                const auto e1 = sub3(v3(ch_.vertexPos[ch_.vertexOfCut[nxt]]), pc);
                const auto e2 = sub3(v3(ch_.vertexPos[ch_.vertexOfCut[prv]]), pc);
                const auto nrm = v3(ch_.faceN[f]);
                const auto ax0 = v3(ch_.faceE0[f]);
                const double n1 = norm3(e1), n2 = norm3(e2);
                if (n1 < 1e-18 || n2 < 1e-18) {
                    continue;  // degenerate wedge
                }
                // UV wedge of this corner: on a folded relaxed map the
                // triangle can be UV-inverted (or the direction can fall
                // outside its UV wedge even though the intrinsic 3D test
                // accepts it) — launching there finds no level exit. The
                // signed UV test below marks candidates whose launch level
                // provably exits the face, and the pool ranks those first
                // (QEx-style: trust signed wedge geometry, not the folded
                // fan).
                const V2 uvA = uv(cut);
                const V2 uvB = uv(nxt);
                const V2 uvC = uv(prv);
                const V2 ue1{uvB.x - uvA.x, uvB.y - uvA.y};
                const V2 ue2{uvC.x - uvA.x, uvC.y - uvA.y};
                const double un1 = std::sqrt(ue1.x * ue1.x + ue1.y * ue1.y);
                const double un2 = std::sqrt(ue2.x * ue2.x + ue2.y * ue2.y);
                const double uvArea2 = ue1.x * ue2.y - ue1.y * ue2.x;
                for (int q = 0; q < 4; ++q) {
                    // d = R_n(q * 90deg) e0 in 3D: e0, n x e0, -e0, -(n x e0).
                    std::array<double, 3> d = ax0;
                    if (q == 1 || q == 3) {
                        d = cross3(nrm, ax0);
                    }
                    if (q >= 2) {
                        d = {-d[0], -d[1], -d[2]};
                    }
                    const double c1 = dot3(nrm, cross3(e1, d)) / n1;
                    const double c2 = dot3(nrm, cross3(d, e2)) / n2;
                    const double margin = std::min(c1, c2);
                    // Chart quarter directions: (1,0),(0,1),(-1,0),(0,-1).
                    const double dqx = q == 0 ? 1.0 : (q == 2 ? -1.0 : 0.0);
                    const double dqy = q == 1 ? 1.0 : (q == 3 ? -1.0 : 0.0);
                    const double uc1 = (ue1.x * dqy - ue1.y * dqx) / std::max(un1, 1e-300);
                    const double uc2 = (dqx * ue2.y - dqy * ue2.x) / std::max(un2, 1e-300);
                    const bool uvOk =
                        uvArea2 > 0.0 && un1 > 1e-18 && un2 > 1e-18 && std::min(uc1, uc2) > -1e-6;
                    // Accept up to ~20deg outside the wedge: near singular
                    // vertices the frame jump across an edge can be large, and
                    // a separatrix direction may fall in the crack between two
                    // adjacent wedge tests. Ranking + dedupe keep the best.
                    // UV-launchable sites are kept regardless (fold rescue): on
                    // a folded fan the intrinsically-correct wedge can be
                    // UV-blocked while the developed line exits a neighboring
                    // wedge of the same quarter class; on clean fans the UV and
                    // 3D wedges agree, so no such extra candidate exists.
                    if (margin <= -0.35 && !uvOk) {
                        continue;
                    }
                    Cand cd;
                    cd.face = f;
                    cd.cut = cut;
                    cd.q = q;
                    cd.margin = margin;
                    cd.edgeKey = 0;
                    if (margin < 0.05) {
                        cd.edgeKey = c1 <= c2 ? canon(cut, nxt) : canon(cut, prv);
                    }
                    cd.uvOk = uvOk;
                    byCone[ch_.vertexOfCut[cut]].push_back(cd);
                }
            }
        }
        std::vector<Walk> walks;
        std::vector<std::pair<std::uint32_t, std::vector<Cand>>> ordered(byCone.begin(),
                                                                         byCone.end());
        std::sort(ordered.begin(), ordered.end(),
                  [](const auto& a, const auto& b) { return a.first < b.first; });
        // Canonical keys of surviving crease seams: directions running along a
        // pinned crease are covered by the crease chains, not by rays.
        std::unordered_set<std::uint64_t> creaseEdgeKeys;
        for (const int s : creaseArcSeam_) {
            const SeamEdge& se = ch_.seams[static_cast<std::size_t>(s)];
            creaseEdgeKeys.insert(pairKey(se.aA, se.bA));
        }
        const auto dir3 = [&](const Cand& cd) {
            const auto nrm = v3(ch_.faceN[cd.face]);
            std::array<double, 3> d = v3(ch_.faceE0[cd.face]);
            if (cd.q == 1 || cd.q == 3) {
                d = cross3(nrm, d);
            }
            if (cd.q >= 2) {
                d = {-d[0], -d[1], -d[2]};
            }
            return d;
        };
        const auto launch = [&](std::uint32_t v, const Cand& cd,
                                const std::vector<LaunchOpt>& alts) {
            Walk wk;
            wk.id = static_cast<int>(walks.size());
            wk.launchFace = cd.face;
            wk.launchCut = cd.cut;
            wk.launchQ = cd.q;
            wk.d3 = dir3(cd);
            wk.alts = alts;
            initWalk(wk);
            Ray ray;
            ray.startNode = vertexNode(v);
            ray.startCut = cd.cut;
            ray.startFace = cd.face;
            ray.q0 = cd.q;
            ray.startAlong = wk.w.s0 * comp(wk.w.p, wk.w.va0);
            ray.startExpr = unitExpr(wk.w.va0 == 0 ? ch_.uIx(cd.cut) : ch_.vIx(cd.cut));
            rays_.push_back(std::move(ray));
            walks.push_back(std::move(wk));
        };
        for (auto& [v, cands] : ordered) {
            const auto itEnds = creaseEndCount_.find(v);
            const int creaseEnds = itEnds == creaseEndCount_.end() ? 0 : itEnds->second;
            std::unordered_map<std::size_t, std::pair<std::size_t, int>> fan;
            int holQcum = std::numeric_limits<int>::min();
            if (!cands.empty()) {
                fanOrder(v, cands.front().face, cands.front().cut, fan, &holQcum);
            }
            // Separatrix count from the seam holonomy when it is known (the
            // map cannot close a layout around any other winding); recorded
            // index otherwise. Identical on substrates where field index and
            // holonomy agree.
            const int recIdx = coneOfVertex_.at(v);
            const int idx = holQcum == std::numeric_limits<int>::min()
                                ? recIdx
                                : liftHolonomyIndex(holQcum, recIdx);
            const int want = 4 - idx - creaseEnds;
            if (want <= 0) {
                continue;  // crease chains cover every sector
            }
            // Developed quarter class per candidate; drop crease-aligned ones.
            struct Scored {
                const Cand* cd;
                int cls;
                std::size_t fanIdx;
            };
            std::vector<Scored> pool;
            for (const Cand& cd : cands) {
                if (cd.edgeKey != 0 && creaseEdgeKeys.count(cd.edgeKey) != 0) {
                    continue;  // runs along a pinned crease: covered by chains
                }
                const auto itF = fan.find(cd.face);
                if (itF == fan.end()) {
                    continue;
                }
                pool.push_back({&cd, ((cd.q - itF->second.second) % 4 + 4) % 4, itF->second.first});
            }
            std::sort(pool.begin(), pool.end(), [](const Scored& a, const Scored& b) {
                // UV-launchable candidates outrank fold-blocked ones: a
                // candidate whose launch level cannot exit its own face in UV
                // burns retries and can exhaust the launch ("no level exit at
                // step=1" on folded cone fans).
                if (a.cd->uvOk != b.cd->uvOk) {
                    return a.cd->uvOk;
                }
                if (a.cd->margin != b.cd->margin) {
                    return a.cd->margin > b.cd->margin;
                }
                return std::tie(a.cd->face, a.cd->q) < std::tie(b.cd->face, b.cd->q);
            });
            // The full separatrix set occupies consecutive quarter classes
            // q0..q0+want-1 (mod 4). Pick the feasible q0 with the best total
            // margin; per class take the top-margin candidates at pairwise fan
            // distance >= 2 (crack double-claims of one ray sit in adjacent
            // wedges; genuine twins do not).
            const std::size_t fanSize = std::max<std::size_t>(fan.size(), 1);
            double bestScore = -std::numeric_limits<double>::infinity();
            std::vector<const Cand*> bestPick;
            const int q0Range = creaseEnds == 0 ? 4 : 4;  // scan all alignments
            for (int q0 = 0; q0 < q0Range; ++q0) {
                std::array<int, 4> need{0, 0, 0, 0};
                for (int j = 0; j < want; ++j) {
                    ++need[static_cast<std::size_t>((q0 + j) % 4)];
                }
                std::vector<const Cand*> pick;
                double score = 0.0;
                bool ok = true;
                for (int c = 0; c < 4 && ok; ++c) {
                    int cnt = need[static_cast<std::size_t>(c)];
                    std::vector<std::pair<std::size_t, double>> chosen;  // (fanIdx, margin)
                    for (const Scored& sc : pool) {
                        if (cnt == 0) {
                            break;
                        }
                        if (sc.cls != c) {
                            continue;
                        }
                        // Fan-adjacent same-class pairs are crack double-claims
                        // of one geometric ray UNLESS both are solidly interior
                        // (genuine twins can sit in adjacent wedges).
                        bool dup = false;
                        for (const auto& [g, gm] : chosen) {
                            const std::size_t dd = (sc.fanIdx + fanSize - g) % fanSize;
                            const bool near = std::min(dd, fanSize - dd) <= 1;
                            if (near && !(sc.cd->margin >= 0.05 && gm >= 0.05)) {
                                dup = true;
                                break;
                            }
                            if (dd == 0) {
                                dup = true;  // same wedge, same class
                                break;
                            }
                        }
                        if (dup) {
                            continue;
                        }
                        pick.push_back(sc.cd);
                        chosen.push_back({sc.fanIdx, sc.cd->margin});
                        // UV-launchable picks dominate the pattern choice (the
                        // bonus is constant when every pick is UV-valid, so
                        // clean fans keep the pure-margin ordering): a pattern
                        // forced through a fold-blocked wedge exhausts its
                        // launch downstream.
                        score += sc.cd->margin + (sc.cd->uvOk ? 1.0 : 0.0);
                        --cnt;
                    }
                    ok = cnt == 0;
                }
                if (ok && score > bestScore) {
                    bestScore = score;
                    bestPick = std::move(pick);
                }
            }
            if (bestPick.size() != static_cast<std::size_t>(want)) {
                if (std::getenv("CYBER_QC_BIMDF_DEBUG") != nullptr) {
                    std::fprintf(stderr,
                                 "[qc] bimdf unfillable cone: v=%u want=%d pool=%zu fan=%zu\n", v,
                                 want, pool.size(), fanSize);
                }
                // Launch nothing structurally consistent: take the best
                // margins so the rotation check reports it downstream.
                for (const Scored& sc : pool) {
                    if (bestPick.size() >= static_cast<std::size_t>(want)) {
                        break;
                    }
                    bestPick.push_back(sc.cd);
                }
            }
            for (const Cand* cd : bestPick) {
                // Fallback launch sites: same-class candidates not chosen for
                // another separatrix, ranked by margin.
                int cls = -1;
                const auto itF = fan.find(cd->face);
                if (itF != fan.end()) {
                    cls = ((cd->q - itF->second.second) % 4 + 4) % 4;
                }
                std::vector<LaunchOpt> alts;
                alts.push_back({cd->face, cd->cut, cd->q, dir3(*cd)});
                for (const Scored& sc : pool) {
                    if (sc.cd == cd || sc.cls != cls) {
                        continue;
                    }
                    if (std::find(bestPick.begin(), bestPick.end(), sc.cd) != bestPick.end()) {
                        continue;
                    }
                    alts.push_back({sc.cd->face, sc.cd->cut, sc.cd->q, dir3(*sc.cd)});
                }
                launch(v, *cd, alts);
            }
        }
        curGen_.assign(walks.size(), 0);
        dependents_.assign(walks.size(), 0);
        // Successful traces are cheap (spot: 3.3k steps for 320 rays); the
        // budget only bounds pathological wanderers, whose per-step cost also
        // grows with the trail density, so keep it tight.
        const std::size_t stepBudget = 10 * ch_.faces.size() + 4096;
        std::size_t stepsUsed = 0;
        std::size_t active = walks.size();
        while (active > 0) {
            if (stepsUsed++ > stepBudget) {
                tm.reason = "trace step budget exceeded (non-terminating separatrices?)";
                return false;
            }
            // Lockstep: advance the ray with the smallest curve parameter.
            Walk* next = nullptr;
            for (Walk& wk : walks) {
                if (!wk.done && (next == nullptr || wk.w.curve < next->w.curve)) {
                    next = &wk;
                }
            }
            const int rc = stepOnce(*next);
            // Abandon a separatrix whose degeneracy cannot be retried away:
            // its ray contributes no arcs, the cone's rotation degrades and
            // the orbits touching it are counted as non-quad patches
            // downstream (the T-mesh is refused with statistics instead of
            // aborting on the first fold).
            const auto abandonRay = [&] {
                Ray& ray = rays_[static_cast<std::size_t>(next->id)];
                ray.failed = true;
                ++failedLaunches_;
                failedLaunchVertex_.insert(ch_.vertexOfCut[next->launchCut]);
                if (std::getenv("CYBER_QC_BIMDF_DEBUG") != nullptr) {
                    std::fprintf(stderr,
                                 "[qc] bimdf abandoned launch: cone-v=%u q=%d face=%zu (%s)\n",
                                 ch_.vertexOfCut[next->launchCut], next->launchQ, next->launchFace,
                                 traceFail_.c_str());
                }
                ray.events.clear();
                ++curGen_[static_cast<std::size_t>(next->id)];  // stale trail
                next->done = true;
                --active;
            };
            if (rc == 1) {
                next->done = true;
                --active;
            } else if (rc == 2) {
                // Numerical degeneracy: retry with a jittered level, allowed
                // only while nothing references this ray's trail. After the
                // jitters, fall over to an alternative launch site of the
                // same separatrix class (UV-degenerate launch faces happen on
                // folded relaxed maps).
                if (dependents_[static_cast<std::size_t>(next->id)] > 0) {
                    // Restarting would orphan the T-nodes other rays placed
                    // on this trail; abandon instead (the placed nodes stay
                    // as spurs on their rays' arcs).
                    abandonRay();
                    continue;
                }
                if (next->attempt >= 3) {
                    if (next->altIdx + 1 < next->alts.size()) {
                        ++next->altIdx;
                        const LaunchOpt& o = next->alts[next->altIdx];
                        next->launchFace = o.face;
                        next->launchCut = o.cut;
                        next->launchQ = o.q;
                        next->d3 = o.d3;
                        next->attempt = 0;
                        Ray& ray = rays_[static_cast<std::size_t>(next->id)];
                        ray.startCut = o.cut;
                        ray.startFace = o.face;
                        ray.q0 = o.q;
                    } else {
                        // Every launch site of this separatrix is fold-blocked
                        // (typically a high-|index| cone whose developed line
                        // rides a seam).
                        abandonRay();
                        continue;
                    }
                } else {
                    ++next->attempt;
                }
                ++curGen_[static_cast<std::size_t>(next->id)];
                {
                    Ray& ray = rays_[static_cast<std::size_t>(next->id)];
                    ray.events.clear();
                    initWalk(*next);
                    ray.startAlong = next->w.s0 * comp(next->w.p, next->w.va0);
                    ray.startExpr = unitExpr(next->w.va0 == 0 ? ch_.uIx(next->launchCut)
                                                              : ch_.vIx(next->launchCut));
                }
            } else if (rc < 0) {
                tm.reason = traceFail_;
                return false;
            }
        }
        tm.raysTraced = rays_.size();
        tm.raySteps = stepsUsed;
        return true;
    }

    bool segDensityOk(std::size_t face) const {
        // A face collecting dozens of trail segments is a wandering /
        // spiralling separatrix (degenerate relaxed map): fail fast instead
        // of paying quadratic crossing scans until the step budget runs out.
        return faceSegs_[face].size() < 64;
    }

    void flushSeg(const Walk& wk, double endVar) {
        const WalkState& ws = wk.w;
        Seg s;
        s.ray = wk.id;
        s.gen = curGen_[static_cast<std::size_t>(wk.id)];
        s.lc = constAxis(ws.q);
        s.c = ws.c;
        s.cExpr = ws.cExpr;
        const double startVar = comp(ws.p, varyAxis(ws.q));
        s.a0 = std::min(startVar, endVar);
        s.a1 = std::max(startVar, endVar);
        s.pvSeg = startVar;
        s.alongEntry = alongOf(ws);
        s.curveEntry = ws.curve;
        s.alongSlope = alongSlopeOf(ws);
        s.Rm0 = invRot(ws.RQ);
        s.T = ws.T;
        s.Tsym = ws.Tsym;
        s.va0 = ws.va0;
        s.s0 = ws.s0;
        faceSegs_[ws.face].push_back(std::move(s));
    }

    // Advance one face-step. Returns 0 = advanced, 1 = terminated,
    // 2 = retry with jitter, -1 = hard fail.
    int stepOnce(Walk& wk) {
        WalkState& w = wk.w;
        Ray& ray = rays_[static_cast<std::size_t>(wk.id)];
        const std::uint32_t startVertex = ch_.vertexOfCut[ray.startCut];
        if (!segDensityOk(w.face)) {
            traceFail_ = "trail density blowup (wandering separatrix)";
            return -1;
        }
        {
            ++wk.steps;
            const int va = varyAxis(w.q);
            const int lca = constAxis(w.q);
            const double pv = comp(w.p, va);
            // Contour-following exit: among the non-entry edges, the level set
            // {coord(lca) == c} crosses exactly one (generic case). At the
            // launch (no entry edge) the crossing consistent with the launch
            // direction is taken. This is fold-robust: no forward test.
            int exitEdge = -1;
            double exitVar = 0.0;
            double bestKey = std::numeric_limits<double>::infinity();
            for (int e = 0; e < 3; ++e) {
                if (e == w.enteredEdge) {
                    continue;
                }
                const V2 A = uv(ch_.faces[w.face][static_cast<std::size_t>(e)]);
                const V2 B = uv(ch_.faces[w.face][static_cast<std::size_t>((e + 1) % 3)]);
                const double da = comp(A, lca) - w.c;
                const double db = comp(B, lca) - w.c;
                if ((da > 0.0 && db > 0.0) || (da < 0.0 && db < 0.0)) {
                    continue;
                }
                const double den = da - db;
                if (std::abs(den) < 1e-18) {
                    continue;  // edge runs along the level
                }
                const double t = da / den;
                if (t < -1e-9 || t > 1.0 + 1e-9) {
                    continue;
                }
                const double xv = comp(A, va) + t * (comp(B, va) - comp(A, va));
                const double delta = xv - pv;
                if (w.enteredEdge < 0) {
                    // Launch: honor the launch direction.
                    const double adv = travelSign(w.q) * delta;
                    if (adv <= 1e-12 || adv >= bestKey) {
                        continue;
                    }
                    bestKey = adv;
                } else {
                    // Interior: skip re-crossings at the entry point itself
                    // (near-vertex entries can duplicate it on another edge).
                    const double dist = std::abs(delta);
                    if (dist <= 1e-9 || dist >= bestKey) {
                        continue;
                    }
                    bestKey = dist;
                }
                exitEdge = e;
                exitVar = xv;
            }
            if (exitEdge < 0 && w.enteredEdge < 0) {
                // Folded launch wedge: the UV-forward test fails even though
                // the direction is intrinsically valid. Pick the level branch
                // whose 3D offset from the cone points along the intrinsic
                // launch direction.
                double bestDot = 0.0;
                for (int e = 0; e < 3; ++e) {
                    const std::size_t ca = ch_.faces[w.face][static_cast<std::size_t>(e)];
                    const std::size_t cb = ch_.faces[w.face][static_cast<std::size_t>((e + 1) % 3)];
                    const V2 A = uv(ca);
                    const V2 B = uv(cb);
                    const double da = comp(A, lca) - w.c;
                    const double db = comp(B, lca) - w.c;
                    if ((da > 0.0 && db > 0.0) || (da < 0.0 && db < 0.0) ||
                        std::abs(da - db) < 1e-18) {
                        continue;
                    }
                    const double t = da / (da - db);
                    if (t < -1e-9 || t > 1.0 + 1e-9) {
                        continue;
                    }
                    const double xv = comp(A, va) + t * (comp(B, va) - comp(A, va));
                    if (std::abs(xv - pv) <= 1e-12) {
                        continue;
                    }
                    const auto pa = ch_.vertexPos[ch_.vertexOfCut[ca]];
                    const auto pb = ch_.vertexPos[ch_.vertexOfCut[cb]];
                    const auto p0 = ch_.vertexPos[ch_.vertexOfCut[wk.launchCut]];
                    std::array<double, 3> off{};
                    for (int j = 0; j < 3; ++j) {
                        off[static_cast<std::size_t>(j)] =
                            (1.0 - t) * pa[static_cast<std::size_t>(j)] +
                            t * pb[static_cast<std::size_t>(j)] - p0[static_cast<std::size_t>(j)];
                    }
                    const double dp = off[0] * wk.d3[0] + off[1] * wk.d3[1] + off[2] * wk.d3[2];
                    if (dp > bestDot) {
                        bestDot = dp;
                        exitEdge = e;
                        exitVar = xv;
                    }
                }
            }
            if (exitEdge < 0 && w.enteredEdge >= 0) {
                // Folded triangle: the level enters and leaves through the
                // SAME edge (the face lies on one side of the level except a
                // sliver at the entry). Take the second crossing of the entry
                // edge; the sign logic below flips the travel direction.
                const V2 A = uv(ch_.faces[w.face][static_cast<std::size_t>(w.enteredEdge)]);
                const V2 B =
                    uv(ch_.faces[w.face][static_cast<std::size_t>((w.enteredEdge + 1) % 3)]);
                const double da = comp(A, lca) - w.c;
                const double db = comp(B, lca) - w.c;
                if (!((da > 0.0 && db > 0.0) || (da < 0.0 && db < 0.0)) &&
                    std::abs(da - db) >= 1e-18) {
                    const double t = da / (da - db);
                    if (t >= -1e-9 && t <= 1.0 + 1e-9) {
                        const double xv = comp(A, va) + t * (comp(B, va) - comp(A, va));
                        if (std::abs(xv - pv) > 1e-9) {
                            exitEdge = w.enteredEdge;
                            exitVar = xv;
                        }
                    }
                }
            }
            if (exitEdge < 0) {
                traceFail_ = "no level exit (face=" + std::to_string(w.face) +
                             " q=" + std::to_string(w.q) + " step=" + std::to_string(wk.steps) +
                             ")";
                return 2;  // numerical degeneracy: retry jittered
            }
            // Local travel direction in this chart (may flip in folds).
            const int sg = exitVar >= pv ? 1 : -1;
            w.q = va == 0 ? (sg > 0 ? 0 : 2) : (sg > 0 ? 1 : 3);
            const int slope = alongSlopeOf(w);
            const double alongEntry = alongOf(w);

            // Crossing another ray's trail inside this face?
            {
                double bestDist = std::numeric_limits<double>::infinity();
                const Seg* hit = nullptr;
                for (const Seg& s : faceSegs_[w.face]) {
                    if (s.ray == wk.id || s.gen != curGen_[static_cast<std::size_t>(s.ray)]) {
                        continue;  // own trail / stale generation
                    }
                    if (s.lc == lca) {
                        // Exactly equal levels are twins / duplicate launches
                        // riding the same developed line: no interaction.
                        // Nearby-but-distinct levels are a tolerance hazard.
                        if (std::abs(s.c - w.c) >= 1e-9 && std::abs(s.c - w.c) < 1e-7) {
                            const double lo = std::min(pv, exitVar), hi = std::max(pv, exitVar);
                            if (s.a1 > lo - 1e-9 && s.a0 < hi + 1e-9) {
                                traceFail_ = "collinear separatrix overlap";
                                return -1;
                            }
                        }
                        continue;
                    }
                    if (w.c < s.a0 - 1e-9 || w.c > s.a1 + 1e-9) {
                        continue;
                    }
                    // Crossing point on my varying axis: their level s.c.
                    const double d1 = sg * (s.c - pv);
                    const double d2 = sg * (exitVar - s.c);
                    if (d1 <= 1e-9 || d2 <= -1e-12) {
                        continue;  // not strictly ahead within this face span
                    }
                    if (d1 < bestDist) {
                        bestDist = d1;
                        hit = &s;
                    }
                }
                if (hit != nullptr) {
                    // Copy: flushSeg below appends to the same vector `hit`
                    // points into (iterator invalidation).
                    const Seg hs = *hit;
                    const std::size_t node = interiorNode();
                    Event ev;
                    ev.along = alongEntry + slope * (hs.c - pv);
                    ev.curve = w.curve + std::abs(hs.c - pv);
                    ev.expr = along0Expr(w, hs.cExpr);
                    ev.node = node;
                    ev.face = w.face;
                    ev.q = w.q;
                    ray.events.push_back(ev);
                    flushSeg(wk, hs.c);
                    // The crossed ray's split event.
                    Event oe;
                    const double varAtHit = w.c;  // their varying coordinate at the crossing
                    oe.along = hs.alongEntry + hs.alongSlope * (varAtHit - hs.pvSeg);
                    oe.curve = hs.curveEntry + std::abs(varAtHit - hs.pvSeg);
                    {
                        const ZExpr* comps[2];
                        comps[hs.lc] = &hs.cExpr;
                        comps[1 - hs.lc] = &w.cExpr;
                        ZExpr out;
                        for (int j = 0; j < 2; ++j) {
                            const double k = static_cast<double>(hs.Rm0.m[hs.va0][j]);
                            if (k == 0.0) {
                                continue;
                            }
                            addScaled(out, *comps[j], k);
                            addScaled(out, hs.Tsym[static_cast<std::size_t>(j)], -k);
                        }
                        pruneExpr(out);
                        oe.expr = std::move(out);
                    }
                    oe.node = node;
                    oe.face = w.face;
                    // Their local travel direction quarter at the hit: varying
                    // axis 1 - s.lc, sign from which end of the segment they
                    // entered at.
                    {
                        const int theirSg = (hs.pvSeg - hs.a0) < (hs.a1 - hs.pvSeg) ? 1 : -1;
                        oe.q = hs.lc == 0 ? (theirSg > 0 ? 1 : 3) : (theirSg > 0 ? 0 : 2);
                    }
                    rays_[static_cast<std::size_t>(hs.ray)].events.push_back(std::move(oe));
                    ++dependents_[static_cast<std::size_t>(hs.ray)];
                    return 1;
                }
            }

            const std::size_t eA = ch_.faces[w.face][static_cast<std::size_t>(exitEdge)];
            const std::size_t eB = ch_.faces[w.face][static_cast<std::size_t>((exitEdge + 1) % 3)];
            // Exact vertex hit at the exit? Only cone and crease-chain
            // vertices are events; a near-hit on a plain regular vertex is a
            // numerical coincidence and the walk passes through (jitter
            // retries are unavailable once other rays reference this trail).
            for (const std::size_t cv : {eA, eB}) {
                const V2 pV = uv(cv);
                if (std::abs(comp(pV, lca) - w.c) < 1e-6 &&
                    std::abs(comp(pV, va) - exitVar) < 1e-6) {
                    if (ch_.vertexOfCut[cv] == startVertex && w.curve + bestKey < 0.5) {
                        traceFail_ = "ray returned to its source cone";
                        return 2;
                    }
                    if (isConeVertex(ch_.vertexOfCut[cv]) ||
                        chainPosOfVertex_.count(ch_.vertexOfCut[cv]) != 0) {
                        return handleVertex(wk, cv) ? 1 : 2;
                    }
                    break;  // regular vertex: continue as a plain edge crossing
                }
            }
            const Nb& n = nbs_[w.face][static_cast<std::size_t>(exitEdge)];
            const int seamIdx = seamOfFaceEdge_[w.face][static_cast<std::size_t>(exitEdge)];
            if (seamIdx >= 0 && creaseArcSeam_.count(seamIdx) != 0) {
                return handleCreaseHit(wk, seamIdx, exitVar) ? 1 : -1;
            }
            if (n.face == kNone) {
                traceFail_ = "ray reached an open boundary";
                return -1;
            }
            // Advance into the neighbor.
            flushSeg(wk, exitVar);
            w.curve += std::abs(exitVar - pv);
            setComp(w.p, lca, w.c);
            setComp(w.p, va, exitVar);
            std::size_t na = eA, nbv = eB;
            if (n.seam >= 0) {
                applyTransition(w, transition(n.seam, n.aToB));
                na = crossCut(n.seam, n.aToB, eA);
                nbv = crossCut(n.seam, n.aToB, eB);
                if (na == kNone || nbv == kNone) {
                    traceFail_ = "seam cut-vertex map failed";
                    return -1;
                }
            }
            w.face = n.face;
            w.enteredEdge = -1;
            for (int e = 0; e < 3; ++e) {
                const std::size_t x = ch_.faces[w.face][static_cast<std::size_t>(e)];
                const std::size_t y = ch_.faces[w.face][static_cast<std::size_t>((e + 1) % 3)];
                if ((x == na && y == nbv) || (x == nbv && y == na)) {
                    w.enteredEdge = e;
                    break;
                }
            }
            return 0;
        }
    }

    // Ray reached a cut vertex exactly. Terminate on cones and crease-chain
    // vertices; regular vertices force a jittered retry.
    bool handleVertex(Walk& wk, std::size_t cv) {
        WalkState& w = wk.w;
        Ray& ray = rays_[static_cast<std::size_t>(wk.id)];
        const std::uint32_t v = ch_.vertexOfCut[cv];
        const int va = varyAxis(w.q);
        const V2 pV = uv(cv);
        const double along = alongOf(w) + alongSlopeOf(w) * (comp(pV, va) - comp(w.p, va));
        const double curve = w.curve + std::abs(comp(pV, va) - comp(w.p, va));
        if (isConeVertex(v)) {
            Event ev;
            ev.along = along;
            ev.curve = curve;
            ev.expr = along0Expr(w, unitExpr(va == 0 ? ch_.uIx(cv) : ch_.vIx(cv)));
            ev.node = vertexNode(v);
            ev.face = w.face;
            ev.q = w.q;
            ev.corner = cv;
            ray.events.push_back(ev);
            flushSeg(wk, comp(pV, va));
            return true;
        }
        const auto itc = chainPosOfVertex_.find(v);
        if (itc == chainPosOfVertex_.end()) {
            traceFail_ = "exact hit on regular vertex " + std::to_string(v);
            return false;
        }
        const std::size_t node = vertexNode(v);
        Event ev;
        ev.along = along;
        ev.curve = curve;
        ev.expr = along0Expr(w, unitExpr(va == 0 ? ch_.uIx(cv) : ch_.vIx(cv)));
        ev.node = node;
        ev.face = w.face;
        ev.q = w.q;
        ev.corner = cv;
        ray.events.push_back(ev);
        flushSeg(wk, comp(pV, va));
        ChainEvent ce;
        ce.along = std::get<1>(itc->second);
        ce.expr = std::get<2>(itc->second);
        ce.node = node;
        ce.edge = std::get<3>(itc->second);
        ce.frac = 0.0;
        chains_[std::get<0>(itc->second)].events.push_back(std::move(ce));
        return true;
    }

    bool handleCreaseHit(Walk& wk, int seamIdx, double exitVar) {
        WalkState& w = wk.w;
        Ray& ray = rays_[static_cast<std::size_t>(wk.id)];
        const SeamEdge& se = ch_.seams[static_cast<std::size_t>(seamIdx)];
        const auto itc = chainEdgeIndex_.find(seamIdx);
        if (itc == chainEdgeIndex_.end()) {
            traceFail_ = "crease seam not in any chain";
            return false;
        }
        const auto [chainId, edgeIdx] = itc->second;
        const Chain& chain = chains_[chainId];
        const ChainEdge& ce = chain.edges[edgeIdx];
        const bool hitFromA = w.face == se.faceA;
        const int creaseVary = 1 - se.pinAxis;  // side-A varying axis along the crease
        // Hit point + its side-A varying-coordinate expression.
        V2 hitA;
        ZExpr hitVaryExprA;
        {
            V2 hit;
            setComp(hit, constAxis(w.q), w.c);
            setComp(hit, varyAxis(w.q), exitVar);
            if (hitFromA) {
                hitA = hit;
                if (creaseVary != constAxis(w.q)) {
                    traceFail_ = "ray parallel to crease at hit";
                    return false;
                }
                hitVaryExprA = w.cExpr;
            } else {
                const Trans toA = transition(seamIdx, false);
                hitA = apply(toA.R, hit);
                hitA.x += toA.tN.x;
                hitA.y += toA.tN.y;
                int srcAxis = -1;
                for (int j = 0; j < 2; ++j) {
                    if (toA.R.m[creaseVary][j] != 0) {
                        srcAxis = j;
                    }
                }
                if (srcAxis != constAxis(w.q)) {
                    traceFail_ = "ray parallel to crease at hit (side B)";
                    return false;
                }
                ZExpr e;
                addScaled(e, w.cExpr, static_cast<double>(toA.R.m[creaseVary][srcAxis]));
                addScaled(e, toA.tS[static_cast<std::size_t>(creaseVary)], 1.0);
                pruneExpr(e);
                hitVaryExprA = std::move(e);
            }
        }
        const std::size_t node = interiorNode();
        // My event: my varying coordinate at the hit equals the crease's
        // pinned coordinate, transported into my chart.
        {
            const std::size_t pinVarA = se.pinAxis == 0 ? ch_.uIx(se.aA) : ch_.vIx(se.aA);
            ZExpr pinExprMine;
            if (hitFromA) {
                pinExprMine = unitExpr(pinVarA);
            } else {
                const Trans toB = transition(seamIdx, true);
                int dstAxis = -1;
                for (int i = 0; i < 2; ++i) {
                    if (toB.R.m[i][se.pinAxis] != 0) {
                        dstAxis = i;
                    }
                }
                ZExpr e;
                addVar(e, pinVarA, static_cast<double>(toB.R.m[dstAxis][se.pinAxis]));
                addScaled(e, toB.tS[static_cast<std::size_t>(dstAxis)], 1.0);
                pruneExpr(e);
                pinExprMine = std::move(e);
            }
            Event ev;
            ev.along = alongOf(w) + alongSlopeOf(w) * (exitVar - comp(w.p, varyAxis(w.q)));
            ev.curve = w.curve + std::abs(exitVar - comp(w.p, varyAxis(w.q)));
            ev.expr = along0Expr(w, pinExprMine);
            ev.node = node;
            ev.face = w.face;
            ev.q = w.q;
            ray.events.push_back(ev);
        }
        flushSeg(wk, exitVar);
        // Chain split event.
        {
            const std::size_t c0 = ce.forward ? se.aA : se.bA;
            const std::size_t c1 = ce.forward ? se.bA : se.aA;
            const double base = chainCoord(se, c0);
            const double h = comp(hitA, creaseVary);
            const double d = h - base;
            const double sgn = (chainCoord(se, c1) - base) >= 0.0 ? 1.0 : -1.0;
            ChainEvent cev;
            cev.along = chain.accum[edgeIdx] + sgn * d;
            ZExpr e = chain.accumExpr[edgeIdx];
            addScaled(e, hitVaryExprA, sgn);
            addVar(e, chainCoordVar(se, c0), -sgn);
            pruneExpr(e);
            cev.expr = std::move(e);
            cev.node = node;
            cev.edge = edgeIdx;
            const double span = std::abs(chainCoord(se, c1) - base);
            cev.frac = span > 1e-18 ? std::min(1.0, std::abs(d) / span) : 0.0;
            chains_[chainId].events.push_back(std::move(cev));
        }
        creaseTNodeAnchor_.emplace(node, std::make_tuple(w.face, seamIdx, hitFromA));
        return true;
    }

    // ---- arc assembly ----------------------------------------------------
    struct ArcEnd {
        std::size_t node = 0;
        std::size_t face = 0;        // anchor chart
        V2 dir;                      // direction pointing AWAY from the node, anchor chart
        std::size_t corner = kNone;  // anchor cut vertex for vertex-fan nodes
    };
    struct ArcRec {
        Arc arc;
        ArcEnd end0, end1;
        int srcRay = -1;  // provenance (diagnostics)
        int srcChain = -1;
        double c0 = 0.0, c1 = 0.0;  // event curve/along bounds (diagnostics)
        double al0 = 0.0, al1 = 0.0;
    };

    void assembleArcs(TMesh& tm) {
        for (std::size_t r = 0; r < rays_.size(); ++r) {
            Ray& ray = rays_[r];
            if (ray.failed) {
                continue;  // abandoned launch: contributes no arcs
            }
            if (ray.events.empty()) {
                tm.reason = "ray without termination event";
                return;
            }
            std::sort(ray.events.begin(), ray.events.end(),
                      [](const Event& a, const Event& b) { return a.curve < b.curve; });
            double prevAlong = ray.startAlong;
            const ZExpr* prevExpr = &ray.startExpr;
            std::size_t prevNode = ray.startNode;
            std::size_t prevFace = ray.startFace;
            int prevQ = ray.q0;
            std::size_t prevCorner = ray.startCut;
            const double s0 = ray.q0 < 2 ? 1.0 : -1.0;
            double prevCurve = 0.0;
            for (const Event& ev : ray.events) {
                ArcRec rec;
                rec.srcRay = static_cast<int>(r);
                rec.c0 = prevCurve;
                rec.c1 = ev.curve;
                rec.al0 = prevAlong;
                rec.al1 = ev.along;
                prevCurve = ev.curve;
                rec.arc.n0 = prevNode;
                rec.arc.n1 = ev.node;
                rec.arc.len = ev.along - prevAlong;
                rec.arc.expr = diffExpr(ev.expr, *prevExpr, s0);
                rec.end0.node = prevNode;
                rec.end0.face = prevFace;
                rec.end0.dir = quarterDir(prevQ);
                rec.end0.corner = prevCorner;
                rec.end1.node = ev.node;
                rec.end1.face = ev.face;
                rec.end1.dir = quarterDir(ev.q + 2);
                rec.end1.corner = ev.corner;
                arcs_.push_back(std::move(rec));
                prevAlong = ev.along;
                prevExpr = &ev.expr;
                prevNode = ev.node;
                prevFace = ev.face;
                prevQ = ev.q;
                prevCorner = kNone;
            }
        }
        for (Chain& chain : chains_) {
            std::sort(chain.events.begin(), chain.events.end(),
                      [](const ChainEvent& a, const ChainEvent& b) { return a.along < b.along; });
            for (std::size_t i = 0; i + 1 < chain.events.size(); ++i) {
                const ChainEvent& e0 = chain.events[i];
                const ChainEvent& e1 = chain.events[i + 1];
                if (e1.along - e0.along < 1e-9 && e0.node == e1.node) {
                    continue;
                }
                ArcRec rec;
                rec.arc.n0 = e0.node;
                rec.arc.n1 = e1.node;
                rec.arc.len = e1.along - e0.along;
                rec.arc.expr = diffExpr(e1.expr, e0.expr, 1.0);
                rec.arc.onFeature = true;
                fillCreaseEndAnchor(chain, e0, true, rec.end0);
                fillCreaseEndAnchor(chain, e1, false, rec.end1);
                rec.end0.node = e0.node;
                rec.end1.node = e1.node;
                arcs_.push_back(std::move(rec));
            }
        }
        // Negative relaxed lengths are REAL on folded relaxed maps (the map is
        // not injective near high-distortion cones): the parametric span of a
        // separatrix piece can locally reverse. The T-mesh stays consistent
        // (opposite patch sides still sum equal), and the quantizer's min-one
        // bound simply pulls such arcs up with deviation cost. Report only.
        for (const ArcRec& rec : arcs_) {
            tm.maxExprErr =
                std::max(tm.maxExprErr, std::abs(evalExpr(rec.arc.expr, z_) - rec.arc.len));
            tm.minArcLen = std::min(tm.minArcLen, rec.arc.len);
        }
        tm.nodeCount = nodes_.size();
        for (const Node& n : nodes_) {
            if (n.type == 1) {
                ++tm.tNodes;
            }
        }
    }

    void fillCreaseEndAnchor(const Chain& chain, const ChainEvent& ev, bool forward, ArcEnd& end) {
        std::size_t edgeIdx = ev.edge;
        if (!forward && ev.frac == 0.0 && edgeIdx > 0) {
            edgeIdx -= 1;  // the arc approaches through the previous chain edge
        }
        if (edgeIdx >= chain.edges.size()) {
            edgeIdx = chain.edges.size() - 1;
        }
        const ChainEdge& ce = chain.edges[edgeIdx];
        const SeamEdge& se = ch_.seams[static_cast<std::size_t>(ce.seam)];
        end.face = se.faceA;
        const std::size_t c0 = ce.forward ? se.aA : se.bA;
        const std::size_t c1 = ce.forward ? se.bA : se.aA;
        V2 d{uv(c1).x - uv(c0).x, uv(c1).y - uv(c0).y};
        const double n = std::hypot(d.x, d.y);
        if (n > 1e-18) {
            d.x /= n;
            d.y /= n;
        }
        if (!forward) {
            d.x = -d.x;
            d.y = -d.y;
        }
        end.dir = d;
        end.corner = forward ? c0 : c1;
    }

    // ---- rotation systems + patch tracing --------------------------------
    struct EndRef {
        std::size_t arc;
        int which;     // 0 = end0, 1 = end1
        int devQ = 0;  // developed quarter direction around the node
        std::size_t fanIdx = 0;
        double local = 0.0;  // metric tiebreak within a fan wedge
        double theta = 0.0;  // developed SIGNED angle around the node (QEx Alg. 8)
    };

    static int quarterOfDir(const V2& d) {
        const double a = std::atan2(d.y, d.x);
        return ((static_cast<int>(std::lround(a / (0.5 * kPiD))) % 4) + 4) % 4;
    }

    // The seam-holonomy cone index (mod 4) measured by a closed fan walk is
    // the authority for tracing: the map cannot close a layout around any
    // other winding. When it disagrees with the field-recorded index, lift
    // the mod-4 class to the index nearest the recorded one (ties toward the
    // smaller magnitude); recorded and measured agree on clean substrates,
    // where this returns `recorded` unchanged.
    static int liftHolonomyIndex(int qcum, int recorded) {
        const int hm = ((qcum % 4) + 4) % 4;
        if (((recorded % 4) + 4) % 4 == hm) {
            return recorded;
        }
        int best = hm;
        for (int c = hm - 8; c <= hm + 8; c += 4) {
            if (std::abs(c - recorded) < std::abs(best - recorded) ||
                (std::abs(c - recorded) == std::abs(best - recorded) &&
                 std::abs(c) < std::abs(best))) {
                best = c;
            }
        }
        return best;
    }

    bool buildPatches(TMesh& tm) {
        std::vector<std::vector<EndRef>> ends(nodes_.size());
        std::vector<std::vector<int>> sectors(nodes_.size());
        for (std::size_t a = 0; a < arcs_.size(); ++a) {
            ends[arcs_[a].end0.node].push_back({a, 0, 0, 0, 0.0});
            ends[arcs_[a].end1.node].push_back({a, 1, 0, 0, 0.0});
        }
        for (std::size_t n = 0; n < nodes_.size(); ++n) {
            if (ends[n].empty()) {
                if (nodes_[n].type == 0 && isConeVertex(nodes_[n].meshVertex) &&
                    failedLaunchVertex_.count(nodes_[n].meshVertex) == 0) {
                    tm.reason = "cone without incident arcs";
                    return false;
                }
                continue;
            }
            if (!buildRotation(n, ends[n], sectors[n], tm)) {
                return false;
            }
        }
        const std::size_t nHalf = 2 * arcs_.size();
        std::vector<char> used(nHalf, 0);
        const auto headOf = [&](std::size_t h) {
            return h % 2 == 0 ? arcs_[h / 2].end1.node : arcs_[h / 2].end0.node;
        };
        for (std::size_t h0 = 0; h0 < nHalf; ++h0) {
            if (used[h0]) {
                continue;
            }
            Patch patch;
            std::vector<std::pair<std::size_t, int>> runs;  // (arc, sector quarters before it)
            std::size_t h = h0;
            bool bad = false;
            std::size_t guard = 0;
            do {
                if (++guard > nHalf + 4) {
                    bad = true;
                    break;
                }
                used[h] = 1;
                const std::size_t node = headOf(h);
                const auto& lst = ends[node];
                std::size_t idx = kNone;
                const int wantWhich = h % 2 == 0 ? 1 : 0;
                for (std::size_t i = 0; i < lst.size(); ++i) {
                    if (lst[i].arc == h / 2 && lst[i].which == wantWhich) {
                        idx = i;
                        break;
                    }
                }
                if (idx == kNone) {
                    tm.reason = "rotation lookup failed";
                    return false;
                }
                const std::size_t j = (idx + lst.size() - 1) % lst.size();
                int quarters = sectors[node][idx];
                if (quarters < 1 || quarters > 2) {
                    quarters = 3;  // poisons the corner count: orbit counted bad
                }
                const std::size_t na = lst[j].arc;
                runs.push_back({na, quarters});
                h = lst[j].which == 0 ? 2 * na : 2 * na + 1;
            } while (h != h0);
            if (bad) {
                tm.reason = "patch orbit did not close";
                return false;
            }
            std::size_t firstCorner = kNone;
            int corners = 0;
            bool cleanSectors = true;
            for (std::size_t i = 0; i < runs.size(); ++i) {
                if (runs[i].second == 1) {
                    ++corners;
                    if (firstCorner == kNone) {
                        firstCorner = i;
                    }
                } else if (runs[i].second != 2) {
                    cleanSectors = false;  // poisoned sector (fold damage)
                }
            }
            // Quads keep the historical acceptance; polygonal patches with
            // 3-6 corners and clean corner/pass-through sectors go to the
            // even-sum interior-routing template (paper Sec. 4.4).
            const bool accept = corners == 4 || (cleanSectors && corners >= 3 && corners <= 6);
            if (!accept) {
                ++badPatches_;
                badCorners_.push_back(corners);
                if (badPatches_ <= 3 && std::getenv("CYBER_QC_BIMDF_DEBUG") != nullptr) {
                    std::fprintf(stderr, "[qc] bimdf bad orbit (%d corners):", corners);
                    for (const auto& [arcId, qq] : runs) {
                        std::fprintf(stderr, " a%zu(s%d,n%zu:%d)", arcId, qq,
                                     arcs_[arcId].end0.node, nodes_[arcs_[arcId].end0.node].type);
                    }
                    std::fprintf(stderr, "\n");
                }
                continue;  // aggregate; fail after the sweep with statistics
            }
            patch.side.assign(static_cast<std::size_t>(corners), {});
            int side = -1;
            for (std::size_t k = 0; k < runs.size(); ++k) {
                const auto& [arcId, q] = runs[(firstCorner + k) % runs.size()];
                if (q == 1) {
                    ++side;
                }
                patch.side[static_cast<std::size_t>(side)].push_back(arcId);
            }
            tm.patches.push_back(std::move(patch));
        }
        if (badPatches_ != 0) {
            // Corner-valence histogram of the rejected orbits ("v:count").
            std::map<int, int> hist;
            for (const int c : badCorners_) {
                ++hist[c];
            }
            std::string cs;
            for (const auto& [val, cnt] : hist) {
                cs += (cs.empty() ? "" : ",") + std::to_string(val) + ":" + std::to_string(cnt);
            }
            tm.reason = "non-quad patches: " + std::to_string(badPatches_) + " of " +
                        std::to_string(tm.patches.size() + badPatches_) + " (corners " + cs + ")";
            return false;
        }
        tm.arcs.reserve(arcs_.size());
        for (ArcRec& rec : arcs_) {
            tm.arcs.push_back(std::move(rec.arc));
        }
        for (const Patch& p : tm.patches) {
            if (!p.isQuad()) {
                continue;  // polygonal patches have no opposite-side pairing
            }
            for (int k = 0; k < 2; ++k) {
                double s0 = 0.0, s1 = 0.0;
                for (const std::size_t a : p.side[static_cast<std::size_t>(k)]) {
                    s0 += tm.arcs[a].len;
                }
                for (const std::size_t a : p.side[static_cast<std::size_t>(k + 2)]) {
                    s1 += tm.arcs[a].len;
                }
                tm.maxSideMismatch = std::max(tm.maxSideMismatch, std::abs(s0 - s1));
            }
        }
        return true;
    }

    // Order the ends of a fully-populated cone by consecutive developed
    // quarter classes: k ends must occupy classes q0, q0+1, ..., q0+k-1
    // (mod 4). Duplicated classes (k > 4: twin separatrices) are assigned to
    // their two slots by fan position (choose the assignment that keeps the
    // fan indices as monotone as possible around the cycle). Returns false
    // when the class multiset does not match the pattern.
    static bool orderConeBySlots(std::vector<EndRef>& lst) {
        const std::size_t k = lst.size();
        if (k < 3 || k > 6) {
            return false;
        }
        std::array<std::vector<std::size_t>, 4> byClass;
        for (std::size_t i = 0; i < k; ++i) {
            byClass[static_cast<std::size_t>(((lst[i].devQ % 4) + 4) % 4)].push_back(i);
        }
        // Find q0: classes q0..q0+k-1 (mod 4) with correct multiplicities.
        for (int q0 = 0; q0 < 4; ++q0) {
            std::array<int, 4> want{0, 0, 0, 0};
            for (std::size_t j = 0; j < k; ++j) {
                ++want[static_cast<std::size_t>((q0 + static_cast<int>(j)) % 4)];
            }
            bool match = true;
            for (int c = 0; c < 4; ++c) {
                if (static_cast<int>(byClass[static_cast<std::size_t>(c)].size()) !=
                    want[static_cast<std::size_t>(c)]) {
                    match = false;
                    break;
                }
            }
            if (!match) {
                continue;
            }
            // Assign slots. For duplicated classes (at most one with k <= 6...
            // k == 6 duplicates two classes), try both assignments per
            // duplicated class and keep the one with fewer fan-order descents.
            std::vector<std::size_t> slot(k, kNone);
            std::vector<int> dupClasses;
            for (std::size_t j = 0; j < k; ++j) {
                const int c = (q0 + static_cast<int>(j)) % 4;
                auto& ids = byClass[static_cast<std::size_t>(c)];
                if (ids.size() == 1) {
                    slot[j] = ids[0];
                } else if (std::find(dupClasses.begin(), dupClasses.end(), c) == dupClasses.end()) {
                    dupClasses.push_back(c);
                }
            }
            // Enumerate assignments for duplicated classes (2 slots, 2 ends
            // each: 2 options per class; at most 2 classes -> 4 options).
            const auto slotsOfClass = [&](int c) {
                std::vector<std::size_t> out;
                for (std::size_t j = 0; j < k; ++j) {
                    if ((q0 + static_cast<int>(j)) % 4 == c) {
                        out.push_back(j);
                    }
                }
                return out;
            };
            std::vector<EndRef> best;
            long bestScore = -1;
            const int nOpt = 1 << dupClasses.size();
            for (int opt = 0; opt < nOpt; ++opt) {
                std::vector<std::size_t> s = slot;
                for (std::size_t dci = 0; dci < dupClasses.size(); ++dci) {
                    const auto slots = slotsOfClass(dupClasses[dci]);
                    const auto& ids = byClass[static_cast<std::size_t>(dupClasses[dci])];
                    const bool flip = ((opt >> dci) & 1) != 0;
                    s[slots[0]] = ids[flip ? 1 : 0];
                    s[slots[1]] = ids[flip ? 0 : 1];
                }
                // Score: forward cyclic fan distance summed over consecutive
                // slots (smaller = more consistent with the fan order).
                long score = 0;
                std::size_t maxFan = 0;
                for (std::size_t j = 0; j < k; ++j) {
                    maxFan = std::max(maxFan, lst[s[j]].fanIdx);
                }
                const std::size_t fs = maxFan + 1;
                for (std::size_t j = 0; j < k; ++j) {
                    const std::size_t a = lst[s[j]].fanIdx;
                    const std::size_t b = lst[s[(j + 1) % k]].fanIdx;
                    score += static_cast<long>((b + fs - a) % fs);
                }
                if (bestScore < 0 || score < bestScore) {
                    bestScore = score;
                    best.clear();
                    for (std::size_t j = 0; j < k; ++j) {
                        best.push_back(lst[s[j]]);
                    }
                }
            }
            lst = std::move(best);
            return true;
        }
        return false;
    }

    // Order the arc ends around node n and compute the sector size (in
    // quarter turns) between each cyclically-consecutive pair. Fully
    // combinatorial: quarter directions in local charts + accumulated seam
    // rotations; no metric angle sums (fold-proof). sect[i] is the sector
    // between ends lst[i-1 (cyclic)] and lst[i]; validated to sum to the
    // node's total quarter winding (4 - cone index).
    bool buildRotation(std::size_t n, std::vector<EndRef>& lst, std::vector<int>& sect, TMesh& tm) {
        const Node& node = nodes_[n];
        int expectedTotal = 4;
        double totalAngle = 0.0;       // raw signed UV angle of the full wedge fan (type-0)
        double totalCorr = 0.0;        // QEx Alg. 8 corrected total (+2pi per negative run)
        std::size_t foldedWedges = 0;  // fan wedges with negative signed UV angle
        if (node.type == 1) {
            // Interior T-node: single chart; crease arc ends anchored on the
            // far seam side are transported by rho. In a fold-flipped face the
            // chart CCW is the surface CW, so the quarters are mirrored before
            // ordering.
            const auto itA = creaseTNodeAnchor_.find(n);
            std::size_t anchorFace = kNone;
            for (EndRef& e : lst) {
                const ArcEnd& end = e.which == 0 ? arcs_[e.arc].end0 : arcs_[e.arc].end1;
                V2 d = end.dir;
                if (itA != creaseTNodeAnchor_.end()) {
                    const auto& [rayFace, seamIdx, hitFromA] = itA->second;
                    anchorFace = rayFace;
                    if (end.face != rayFace) {
                        const SeamEdge& se = ch_.seams[static_cast<std::size_t>(seamIdx)];
                        if (!hitFromA && end.face == se.faceA) {
                            d = apply(rotPow(se.rho), d);
                        }
                    }
                } else if (anchorFace == kNone) {
                    anchorFace = end.face;
                }
                e.devQ = quarterOfDir(d);
                e.fanIdx = 0;
                e.local = static_cast<double>(e.devQ);
            }
            const auto faceSign = [&](std::size_t f2) {
                const V2 a = uv(ch_.faces[f2][0]);
                const V2 b = uv(ch_.faces[f2][1]);
                const V2 c = uv(ch_.faces[f2][2]);
                return (b.x - a.x) * (c.y - a.y) - (b.y - a.y) * (c.x - a.x) >= 0.0 ? 1 : -1;
            };
            // A T-node on a fold boundary anchors ends in faces of OPPOSITE
            // UV orientation; a single whole-chart mirror cannot express
            // that. Normalize minority-orientation ends by reflecting their
            // quarter about the through-ray axis (a fold is a reflection,
            // and the shared level line is its axis).
            int refSign = anchorFace != kNone ? faceSign(anchorFace) : 1;
            {
                int pos = 0, neg = 0;
                std::vector<int> signs(lst.size(), 1);
                for (std::size_t i = 0; i < lst.size(); ++i) {
                    const EndRef& e = lst[i];
                    const ArcEnd& end = e.which == 0 ? arcs_[e.arc].end0 : arcs_[e.arc].end1;
                    signs[i] = faceSign(end.face);
                    (signs[i] > 0 ? pos : neg) += 1;
                }
                if (pos != 0 && neg != 0) {
                    refSign = pos >= neg ? 1 : -1;
                    int axis = -1;
                    for (std::size_t i = 0; i < lst.size() && axis < 0; ++i) {
                        for (std::size_t j = i + 1; j < lst.size() && axis < 0; ++j) {
                            if (signs[i] == refSign && signs[j] == refSign &&
                                ((lst[i].devQ - lst[j].devQ) % 4 + 4) % 4 == 2) {
                                axis = lst[i].devQ % 2;  // the through pair's axis
                            }
                        }
                    }
                    if (axis < 0) {
                        for (std::size_t i = 0; i < lst.size() && axis < 0; ++i) {
                            if (signs[i] == refSign) {
                                axis = lst[i].devQ % 2;
                            }
                        }
                    }
                    for (std::size_t i = 0; i < lst.size(); ++i) {
                        if (signs[i] != refSign) {
                            lst[i].devQ =
                                axis == 0 ? (4 - lst[i].devQ) % 4 : (2 - lst[i].devQ + 4) % 4;
                        }
                    }
                }
            }
            if (refSign < 0) {
                for (EndRef& e : lst) {
                    e.devQ = (4 - e.devQ) % 4;  // mirror the chart
                }
            }
            std::sort(lst.begin(), lst.end(),
                      [](const EndRef& a, const EndRef& b) { return a.devQ < b.devQ; });
        } else {
            const std::uint32_t v = node.meshVertex;
            // Seed the fan from any end's anchor.
            std::size_t f0 = kNone, c0 = kNone;
            for (const EndRef& e : lst) {
                const ArcEnd& end = e.which == 0 ? arcs_[e.arc].end0 : arcs_[e.arc].end1;
                for (int k = 0; k < 3; ++k) {
                    const std::size_t cut = ch_.faces[end.face][static_cast<std::size_t>(k)];
                    if (ch_.vertexOfCut[cut] == v) {
                        f0 = end.face;
                        c0 = end.corner != kNone ? end.corner : cut;
                    }
                }
                if (f0 != kNone) {
                    break;
                }
            }
            if (f0 == kNone) {
                tm.reason = "fan seed not found";
                return false;
            }
            // Walk the face fan, accumulating the seam quarter rotation of
            // each wedge chart relative to the seed chart.
            struct FanFace {
                std::size_t face;
                std::size_t cornerCut;
                int qcum;
                V2 startEdge;
                double cum;  // developed signed angle at this wedge's start edge
                double phi;  // signed inner UV angle of this wedge
            };
            std::vector<FanFace> fan;
            std::size_t f = f0, cornerCut = c0;
            int qcum = 0;
            std::size_t guard = 0;
            while (true) {
                if (++guard > ch_.faces.size() + 4) {
                    tm.reason = "fan walk did not close";
                    return false;
                }
                int corner = -1;
                for (int k = 0; k < 3; ++k) {
                    if (ch_.faces[f][static_cast<std::size_t>(k)] == cornerCut) {
                        corner = k;
                    }
                }
                if (corner < 0) {
                    tm.reason = "fan corner lost";
                    return false;
                }
                const V2 pc = uv(cornerCut);
                const std::size_t nxt = ch_.faces[f][static_cast<std::size_t>((corner + 1) % 3)];
                const std::size_t prv = ch_.faces[f][static_cast<std::size_t>((corner + 2) % 3)];
                const V2 e1{uv(nxt).x - pc.x, uv(nxt).y - pc.y};
                const V2 e2{uv(prv).x - pc.x, uv(prv).y - pc.y};
                // Signed inner UV angle of this wedge (negative when the
                // triangle is UV-folded): the accumulated sum develops the fan
                // with folds cancelling their positive double cover (QEx,
                // Ebke et al. 2013, Algorithm 8).
                const double phi = std::atan2(e1.x * e2.y - e1.y * e2.x, e1.x * e2.x + e1.y * e2.y);
                fan.push_back({f, cornerCut, qcum, e1, totalAngle, phi});
                if (phi < 0.0) {
                    ++foldedWedges;
                }
                totalAngle += phi;
                const int le = (corner + 2) % 3;  // edge (prv -> corner)
                const Nb& nbr = nbs_[f][static_cast<std::size_t>(le)];
                if (nbr.face == kNone) {
                    tm.reason = "fan hit boundary";
                    return false;
                }
                std::size_t nextCorner = cornerCut;
                if (nbr.seam >= 0) {
                    nextCorner = crossCut(nbr.seam, nbr.aToB, cornerCut);
                    if (nextCorner == kNone) {
                        tm.reason = "fan seam corner map failed";
                        return false;
                    }
                    const int rho = ch_.seams[static_cast<std::size_t>(nbr.seam)].rho;
                    qcum += nbr.aToB ? rho : -rho;
                }
                if (nbr.face == f0 && nextCorner == c0) {
                    break;
                }
                f = nbr.face;
                cornerCut = nextCorner;
            }
            // QEx Algorithm 8 (Ebke et al. 2013): split the wedge fan into
            // maximal uniform-sign runs. A negative run is a fold-back whose
            // intended development still advances FORWARD around the vertex by
            // 2pi + phi_run (the fold covers its angular range the long way
            // round); positive runs advance by their raw angle. The corrected
            // accumulation recovers the true sector geometry from a folded fan
            // (measured: it reproduces the seam-holonomy winding at every
            // fold-collapsed cone, where the raw signed sum loses full turns).
            struct FanRun {
                std::size_t first;  // first fan face of the run
                double phi;         // raw signed angle of the run
                double cumCorr;     // corrected developed angle at run start
                bool fullTurn;      // fold-back covering its range the long way round
            };
            std::vector<FanRun> fanRuns;
            std::vector<std::size_t> runOf(fan.size(), 0);
            std::vector<double> phiBefore(fan.size(), 0.0);  // raw prefix within the run
            for (std::size_t i = 0; i < fan.size(); ++i) {
                const bool neg = fan[i].phi < 0.0;
                if (fanRuns.empty() || neg != (fanRuns.back().phi < 0.0)) {
                    fanRuns.push_back({i, 0.0, 0.0, false});
                }
                runOf[i] = fanRuns.size() - 1;
                phiBefore[i] = fanRuns.back().phi;
                fanRuns.back().phi += fan[i].phi;
            }
            // The raw signed sum determines the winding only mod 2pi: each
            // negative run MAY be a full-turn fold-back (QEx Fig. 2d, +2pi)
            // or a small re-covered backtrack (+0). Lift the winding to the
            // smallest total that can seat every incident end with a
            // corner/pass-through sector, and charge the turns to the
            // deepest fold-backs.
            std::vector<std::size_t> negIdx;
            for (std::size_t r = 0; r < fanRuns.size(); ++r) {
                if (fanRuns[r].phi < 0.0) {
                    negIdx.push_back(r);
                }
            }
            const long long rawQ =
                std::llround(totalAngle / (0.5 * kPiD));  // exact mod-4 residue carrier
            long long turns = 0;
            while (turns < static_cast<long long>(negIdx.size()) &&
                   rawQ + 4 * turns < static_cast<long long>(lst.size())) {
                ++turns;
            }
            std::stable_sort(negIdx.begin(), negIdx.end(), [&](std::size_t a, std::size_t b) {
                return fanRuns[a].phi < fanRuns[b].phi;
            });
            for (long long t = 0; t < turns; ++t) {
                fanRuns[negIdx[static_cast<std::size_t>(t)]].fullTurn = true;
            }
            for (FanRun& r : fanRuns) {
                r.cumCorr = totalCorr;
                totalCorr += r.fullTurn ? 2.0 * kPiD + r.phi : r.phi;
            }
            // Anchor ends and compute developed quarters.
            for (EndRef& e : lst) {
                const ArcEnd& end = e.which == 0 ? arcs_[e.arc].end0 : arcs_[e.arc].end1;
                const FanFace* ff = nullptr;
                std::size_t idx = 0;
                for (std::size_t i = 0; i < fan.size(); ++i) {
                    if (fan[i].face == end.face &&
                        (end.corner == kNone || fan[i].cornerCut == end.corner)) {
                        ff = &fan[i];
                        idx = i;
                        break;
                    }
                }
                if (ff == nullptr) {
                    tm.reason = "arc end not anchored in fan";
                    return false;
                }
                e.devQ = ((quarterOfDir(end.dir) - ff->qcum) % 4 + 4) % 4;
                e.fanIdx = idx;
                e.local = std::atan2(ff->startEdge.x * end.dir.y - ff->startEdge.y * end.dir.x,
                                     ff->startEdge.x * end.dir.x + ff->startEdge.y * end.dir.y);
                // Corrected developed angle of this end. Inside a full-turn
                // fold-back the metric order is reversed relative to the
                // surface order; the proportional map keeps it monotone in
                // the corrected (forward) development. Small backtracks keep
                // their raw (physical) placement.
                const FanRun& run = fanRuns[runOf[idx]];
                const double off = phiBefore[idx] + e.local;
                if (run.fullTurn) {
                    const double frac = std::clamp(off / run.phi, 0.0, 1.0);
                    e.theta = run.cumCorr + (2.0 * kPiD + run.phi) * frac;
                } else {
                    e.theta = run.cumCorr + off;
                }
            }
            std::sort(lst.begin(), lst.end(), [](const EndRef& a, const EndRef& b) {
                if (a.fanIdx != b.fanIdx) {
                    return a.fanIdx < b.fanIdx;
                }
                return a.local < b.local;
            });
            // Folded wedges reverse the metric order of two ends sharing a
            // wedge; their developed quarters expose it (a CCW-consecutive
            // pair can never differ by -1 quarter).
            for (std::size_t i = 1; i < lst.size(); ++i) {
                if (lst[i].fanIdx == lst[i - 1].fanIdx &&
                    ((lst[i].devQ - lst[i - 1].devQ) % 4 + 4) % 4 == 3) {
                    std::swap(lst[i], lst[i - 1]);
                }
            }
            const auto itCone = coneOfVertex_.find(v);
            const int recIdx = itCone != coneOfVertex_.end() ? itCone->second : 0;
            expectedTotal = 4 - liftHolonomyIndex(qcum, recIdx);
            if (foldedWedges == 0 && itCone != coneOfVertex_.end() &&
                lst.size() == static_cast<std::size_t>(expectedTotal) && orderConeBySlots(lst)) {
                // Cone with its full separatrix complement: exactly one arc
                // end per grid sector, every sector is one quarter, and the
                // ends occupy consecutive developed-quarter classes. Twins
                // (duplicated class when 4-s > 4) are disambiguated by fan
                // position; everything else is exact quarter arithmetic.
                sect.assign(lst.size(), 1);
                return true;
            }
        }
        // Sector quarters between cyclically-consecutive ends. Mod-4 diffs
        // telescope to 0 around the cycle and lose the cone holonomy, so the
        // non-wrap gaps are taken mod 4 and the wrap gap is derived from the
        // known total winding.
        sect.assign(lst.size(), 0);
        int sum = 0;
        for (std::size_t i = 1; i < lst.size(); ++i) {
            const int gap = ((lst[i].devQ - lst[i - 1].devQ) % 4 + 4) % 4;
            sect[i] = gap == 0 ? 4 : gap;
            sum += sect[i];
        }
        sect[0] = expectedTotal - sum;
        sum += sect[0];
        const int wrapMax = lst.size() == 1 ? 4 : 2;
        bool valid = sect[0] >= 1 && sect[0] <= wrapMax;
        if (node.type == 1) {
            if (!valid) {
                // Winding inconsistency (fold-damaged neighborhood): keep the
                // rotation as ordered; the orbits touching it are counted as
                // non-quad patches downstream instead of failing the whole
                // build.
                ++degradedNodes_;
                if (std::getenv("CYBER_QC_BIMDF_DEBUG") != nullptr) {
                    std::fprintf(stderr,
                                 "[qc] bimdf rotation mismatch: node=%zu type=%d ends=%zu sum=%d "
                                 "expected=%d\n",
                                 n, node.type, lst.size(), sum, expectedTotal);
                }
            }
            return true;
        }
        // Vertex-fan node: the quarter arithmetic is exact on clean fans, but
        // a folded wedge corrupts the developed quarters (the chart CCW is the
        // surface CW inside the fold) WITHOUT necessarily leaving the [1,2]
        // range, which poisons corner sectors downstream into degenerate
        // orbits. Accept the exact result only on fold-free fans with every
        // sector a plausible corner (1) or pass-through (2).
        for (std::size_t i = 1; valid && i < lst.size(); ++i) {
            valid = sect[i] >= 1 && sect[i] <= 2;
        }
        if (valid && foldedWedges == 0) {
            return true;
        }
        // QEx Algorithm 8 (Ebke et al. 2013) signed-angle reclassification:
        // order the ends by their corrected developed angle (folded runs
        // advance forward by 2pi + phi) and re-derive the sector quarters
        // from the angle gaps. The wrap sector comes from the corrected
        // total, which is the fan's true winding even when the raw signed
        // sum lost full turns (QEx Fig. 2d valence collapse) or when the
        // recorded cone index disagrees with the seam holonomy.
        std::stable_sort(lst.begin(), lst.end(),
                         [](const EndRef& a, const EndRef& b) { return a.theta < b.theta; });
        const int measuredTotal = static_cast<int>(std::llround(totalCorr / (0.5 * kPiD)));
        constexpr double kQuarterEps = 0.6;  // ambiguity guard, in quarters (the
                                             // proportional map inside folded runs
                                             // wobbles ports off the quarter grid;
                                             // the polytope projection absorbs it)
        // Real-valued gaps in quarters (index 0 is the wrap gap), rounded to
        // integers under the constraint that they sum to the measured total
        // (largest-remainder adjustment instead of dumping all rounding error
        // into the wrap gap).
        std::vector<double> gq(lst.size(), 0.0);
        for (std::size_t i = 1; i < lst.size(); ++i) {
            gq[i] = (lst[i].theta - lst[i - 1].theta) / (0.5 * kPiD);
        }
        gq[0] = totalCorr / (0.5 * kPiD);
        for (std::size_t i = 1; i < lst.size(); ++i) {
            gq[0] -= gq[i];
        }
        const std::size_t nE = lst.size();
        if (measuredTotal == static_cast<int>(nE) && nE > 0) {
            // Exactly one quarter per end is the ONLY assignment with every
            // sector in the corner/pass-through range: take it directly (the
            // theta sort still fixes the rotation order).
            sect.assign(nE, 1);
            ++repairedNodes_;
            return true;
        }
        // Largest-remainder rounding under the sum constraint: round every
        // gap to the nearest integer, then walk the remaining deficit or
        // surplus onto the gaps with the largest rounding error instead of
        // dumping it all into the wrap gap. The [1,2] corner/pass-through
        // range stays a VALIDITY CHECK, not a constraint — a measured 0-gap
        // (coincident ports) or 3-gap is real fold damage and forcing it
        // into range corrupts neighboring orbits.
        sect.assign(nE, 0);
        sum = 0;
        for (std::size_t i = 0; i < nE; ++i) {
            sect[i] = static_cast<int>(std::llround(gq[i]));
            sum += sect[i];
        }
        for (int guard2 = 0; sum != measuredTotal && guard2 < 64; ++guard2) {
            const int dir = sum < measuredTotal ? 1 : -1;
            std::size_t pick = 0;
            double bestGain = -1e30;
            for (std::size_t i = 0; i < nE; ++i) {
                const double gain =
                    static_cast<double>(dir) * (gq[i] - static_cast<double>(sect[i]));
                if (gain > bestGain) {
                    bestGain = gain;
                    pick = i;
                }
            }
            sect[pick] += dir;
            sum += dir;
        }
        double maxErr = std::abs(totalCorr / (0.5 * kPiD) - static_cast<double>(measuredTotal));
        for (std::size_t i = 0; i < nE; ++i) {
            maxErr = std::max(maxErr, std::abs(gq[i] - static_cast<double>(sect[i])));
        }
        bool repaired = maxErr <= kQuarterEps && sect[0] >= 1 && sect[0] <= wrapMax;
        for (std::size_t i = 1; repaired && i < nE; ++i) {
            repaired = sect[i] >= 1 && sect[i] <= 2;
        }
        if (repaired) {
            ++repairedNodes_;
        } else {
            // Keep the reclassified rotation (it is at least as consistent as
            // the corrupted quarter arithmetic); orbits touching it are
            // counted as non-quad patches downstream.
            ++degradedNodes_;
            if (std::getenv("CYBER_QC_BIMDF_DEBUG") != nullptr) {
                std::fprintf(stderr,
                             "[qc] bimdf rotation mismatch: node=%zu type=%d ends=%zu "
                             "measured=%d expected=%d maxErr=%.2f\n",
                             n, node.type, lst.size(), measuredTotal, expectedTotal, maxErr);
            }
        }
        return true;
    }

    const Charts& ch_;
    const std::vector<double>& z_;
    std::vector<std::array<Nb, 3>> nbs_;
    std::vector<std::array<int, 3>> seamOfFaceEdge_;
    std::vector<Node> nodes_;
    std::unordered_map<std::uint32_t, std::size_t> vertexNode_;
    std::unordered_map<std::uint32_t, int> coneOfVertex_;
    std::vector<Chain> chains_;
    std::unordered_map<int, std::pair<std::size_t, std::size_t>> chainEdgeIndex_;
    std::unordered_set<int> creaseArcSeam_;  // seams that belong to surviving chains
    std::unordered_map<std::uint32_t, int> creaseEndCount_;  // chain ends per mesh vertex
    std::unordered_map<std::uint32_t, std::tuple<std::size_t, double, ZExpr, std::size_t>>
        chainPosOfVertex_;
    std::unordered_map<std::size_t, std::tuple<std::size_t, int, bool>> creaseTNodeAnchor_;
    std::vector<std::vector<Seg>> faceSegs_;
    std::vector<Ray> rays_;
    std::vector<int> curGen_;      // per-ray trail generation
    std::vector<int> dependents_;  // per-ray count of T-nodes others placed on it
    std::vector<ArcRec> arcs_;
    std::string traceFail_;
    std::size_t openEdges_ = 0;
    std::size_t badPatches_ = 0;
    std::size_t degradedNodes_ = 0;
    std::size_t repairedNodes_ = 0;
    std::size_t failedLaunches_ = 0;
    std::unordered_set<std::uint32_t> failedLaunchVertex_;
    std::vector<int> badCorners_;
};

}  // namespace

TMesh buildTMesh(const Charts& charts, const std::vector<double>& zRelaxed) {
    Builder b(charts, zRelaxed);
    return b.run();
}

double deviationEnergy(const TMesh& tm, const std::vector<double>& arcLenCells) {
    double e = 0.0;
    for (std::size_t a = 0; a < tm.arcs.size(); ++a) {
        e += std::abs(arcLenCells[a] - tm.arcs[a].len) / std::max(tm.arcs[a].len, 0.5);
    }
    return e;
}

// ---------------------------------------------------------------------------
// Bi-MDF S1 solve (Heistermann et al. 2023, approximate path): split-node
// template per quad patch, deviation reformulation around the rounded guess,
// T-join parity adjustment (spanning-forest + DFS), Hochbaum double cover to
// an ordinary min-cost flow (SPFA SSP with convex piecewise-linear deviation
// costs as parallel arcs). Units are HALF-CELLS: under the solver's reduction
// cones live on the half-integer lattice, so arc lengths are half-integers.
// ---------------------------------------------------------------------------
BimdfResult solveBimdf(const TMesh& tm) {
    BimdfResult r;
    const std::size_t nArc = tm.arcs.size();
    const std::size_t nPatch = tm.patches.size();
    if (nArc == 0 || nPatch == 0) {
        r.reason = "empty T-mesh";
        return r;
    }
    // Split-node template: one node per patch side; every arc is a head-head
    // bi-edge between the side nodes of its two incident patch sides. Quads
    // pair opposite sides with a tail-tail inner edge whose flow is the
    // quantized side length; polygonal patches (3-6 sides) get one interior
    // node, a tail-tail routing edge per side and a head-head even-sum loop
    // at the interior node (its sigma of +2 makes the boundary sum
    // 2 * loopFlow — even by construction; Heistermann et al. 2023 Sec 4.4).
    std::vector<std::size_t> sideBase(nPatch, 0);
    std::size_t nNodeCount = 0;
    for (std::size_t p = 0; p < nPatch; ++p) {
        sideBase[p] = nNodeCount;
        nNodeCount += tm.patches[p].side.size();
    }
    std::vector<std::size_t> interiorOf(nPatch, kNone);
    for (std::size_t p = 0; p < nPatch; ++p) {
        if (!tm.patches[p].isQuad()) {
            interiorOf[p] = nNodeCount++;
            ++r.polyPatches;
        }
    }
    const std::size_t nNode = nNodeCount;
    std::vector<std::array<long long, 2>> arcNode(nArc, {-1, -1});
    for (std::size_t p = 0; p < nPatch; ++p) {
        for (std::size_t k = 0; k < tm.patches[p].side.size(); ++k) {
            const long long node = static_cast<long long>(sideBase[p] + k);
            for (const std::size_t a : tm.patches[p].side[k]) {
                if (arcNode[a][0] < 0) {
                    arcNode[a][0] = node;
                } else if (arcNode[a][1] < 0) {
                    arcNode[a][1] = node;
                } else {
                    r.reason = "arc incident to more than two patch sides";
                    return r;
                }
            }
        }
    }
    for (std::size_t a = 0; a < nArc; ++a) {
        if (arcNode[a][1] < 0) {
            r.reason = "arc not incident to two patch sides";
            return r;
        }
    }
    // Targets (half-cells) and initial rounded guess with the min-one guard.
    std::vector<double> target(nArc);
    std::vector<long long> g(nArc);
    for (std::size_t a = 0; a < nArc; ++a) {
        target[a] = 2.0 * tm.arcs[a].len;
        g[a] = std::max<long long>(1, std::llround(target[a]));
        if (static_cast<double>(g[a]) - target[a] > 0.5001) {
            ++r.raisedToMin;  // degenerate-assignment guard (short/negative arc)
        }
    }
    // Inner edges. Quads: one tail-tail edge per opposite side pair, guess =
    // mean of the side sums. Polygonal patches: one tail-tail routing edge
    // per side (guess = the side's arc sum) plus one head-head loop at the
    // interior node (guess = half the routed boundary; sigma +2).
    struct InnerEdge {
        long long n0, n1;  // endpoints (n0 == n1: the interior even-sum loop)
        long long g;       // guess
        double t;          // relaxed target
        bool loop;
    };
    std::vector<InnerEdge> inner;
    for (std::size_t p = 0; p < nPatch; ++p) {
        const Patch& patch = tm.patches[p];
        if (patch.isQuad()) {
            for (int j = 0; j < 2; ++j) {
                long long s0 = 0, s1 = 0;
                double t0 = 0.0;
                for (const std::size_t a : patch.side[static_cast<std::size_t>(j)]) {
                    s0 += g[a];
                    t0 += target[a];
                }
                for (const std::size_t a : patch.side[static_cast<std::size_t>(j + 2)]) {
                    s1 += g[a];
                    t0 += target[a];
                }
                inner.push_back(
                    {static_cast<long long>(sideBase[p] + static_cast<std::size_t>(j)),
                     static_cast<long long>(sideBase[p] + static_cast<std::size_t>(j) + 2),
                     std::max<long long>(1, (s0 + s1 + 1) / 2), t0 * 0.5, false});
            }
        } else {
            long long boundary = 0;
            double tBoundary = 0.0;
            for (std::size_t k = 0; k < patch.side.size(); ++k) {
                long long s0 = 0;
                double t0 = 0.0;
                for (const std::size_t a : patch.side[k]) {
                    s0 += g[a];
                    t0 += target[a];
                }
                inner.push_back({static_cast<long long>(sideBase[p] + k),
                                 static_cast<long long>(interiorOf[p]), std::max<long long>(1, s0),
                                 t0, false});
                boundary += std::max<long long>(1, s0);
                tBoundary += t0;
            }
            inner.push_back(
                {static_cast<long long>(interiorOf[p]), static_cast<long long>(interiorOf[p]),
                 std::max<long long>(1, std::llround(0.5 * static_cast<double>(boundary))),
                 0.5 * tBoundary, true});
        }
    }
    const std::size_t nInner = inner.size();
    // Residual demands b = -(sigma g): arcs inject at both heads, inner edges
    // drain at both tails; the even-sum loop is head-head with sigma +2.
    std::vector<long long> b(nNode, 0);
    const auto recomputeB = [&]() {
        std::fill(b.begin(), b.end(), 0);
        for (std::size_t a = 0; a < nArc; ++a) {
            b[static_cast<std::size_t>(arcNode[a][0])] -= g[a];
            b[static_cast<std::size_t>(arcNode[a][1])] -= g[a];
        }
        for (const InnerEdge& e : inner) {
            if (e.loop) {
                b[static_cast<std::size_t>(e.n0)] -= 2 * e.g;
            } else {
                b[static_cast<std::size_t>(e.n0)] += e.g;
                b[static_cast<std::size_t>(e.n1)] += e.g;
            }
        }
    };
    recomputeB();
    // Convex relative-deviation cost, scaled to integers.
    constexpr double kCostScale = 256.0;
    const auto cost = [&](std::size_t a, long long x) {
        return std::abs(static_cast<double>(x) - target[a]) / std::max(target[a], 1.0);
    };
    // T-join parity adjustment: flip g by +-1 along a forest so every demand
    // becomes even (Algorithm 1 of the paper: spanning forest under the flip
    // costs, DFS post-order sweep).
    {
        struct TEdge {
            long long u, v;
            double w;
            std::size_t arc;  // kNone: inner edge index in `inner`
            std::size_t inner;
        };
        std::vector<TEdge> edges;
        edges.reserve(nArc + nInner);
        for (std::size_t a = 0; a < nArc; ++a) {
            const double c0 =
                std::min(cost(a, g[a] + 1), g[a] > 1 ? cost(a, g[a] - 1) : 1e30) - cost(a, g[a]);
            edges.push_back({arcNode[a][0], arcNode[a][1], c0, a, kNone});
        }
        for (std::size_t ii = 0; ii < nInner; ++ii) {
            if (inner[ii].loop) {
                continue;  // sigma +-2: flipping never changes any parity
            }
            edges.push_back({inner[ii].n0, inner[ii].n1, 0.0, kNone, ii});
        }
        std::sort(edges.begin(), edges.end(), [](const TEdge& a, const TEdge& b) {
            if (a.w != b.w) {
                return a.w < b.w;
            }
            return std::tie(a.u, a.v, a.arc, a.inner) < std::tie(b.u, b.v, b.arc, b.inner);
        });
        std::vector<std::size_t> parent(nNode);
        for (std::size_t i = 0; i < nNode; ++i) {
            parent[i] = i;
        }
        std::function<std::size_t(std::size_t)> find = [&](std::size_t x) {
            while (parent[x] != x) {
                parent[x] = parent[parent[x]];
                x = parent[x];
            }
            return x;
        };
        std::vector<std::vector<std::pair<std::size_t, std::size_t>>> tree(nNode);  // (nbr, edge)
        for (std::size_t e = 0; e < edges.size(); ++e) {
            const std::size_t ru = find(static_cast<std::size_t>(edges[e].u));
            const std::size_t rv = find(static_cast<std::size_t>(edges[e].v));
            if (ru == rv) {
                continue;
            }
            parent[ru] = rv;
            tree[static_cast<std::size_t>(edges[e].u)].push_back(
                {static_cast<std::size_t>(edges[e].v), e});
            tree[static_cast<std::size_t>(edges[e].v)].push_back(
                {static_cast<std::size_t>(edges[e].u), e});
        }
        // DFS post-order: flip the tree edge of every child whose subtree
        // parity is odd. Flipping a bi-edge toggles BOTH endpoint parities.
        std::vector<char> visited(nNode, 0);
        std::vector<char> odd(nNode, 0);
        for (std::size_t i = 0; i < nNode; ++i) {
            odd[i] = static_cast<char>(b[i] & 1);
        }
        const auto flipEdge = [&](const TEdge& te) {
            ++r.parityFlips;
            if (te.arc != kNone) {
                const std::size_t a = te.arc;
                const bool canDown = g[a] > 1;
                if (canDown && cost(a, g[a] - 1) <= cost(a, g[a] + 1)) {
                    --g[a];
                } else {
                    ++g[a];
                }
            } else {
                InnerEdge& e = inner[te.inner];
                if (e.g > 1 && std::abs(static_cast<double>(e.g - 1) - e.t) <=
                                   std::abs(static_cast<double>(e.g + 1) - e.t)) {
                    --e.g;
                } else {
                    ++e.g;
                }
            }
        };
        for (std::size_t root = 0; root < nNode; ++root) {
            if (visited[root]) {
                continue;
            }
            // Iterative DFS collecting post-order.
            std::vector<std::tuple<std::size_t, std::size_t, std::size_t>>
                stack;  // node,parent,edge
            std::vector<std::tuple<std::size_t, std::size_t, std::size_t>> post;
            stack.push_back({root, kNone, kNone});
            visited[root] = 1;
            while (!stack.empty()) {
                const auto [n, par, pe] = stack.back();
                stack.pop_back();
                post.push_back({n, par, pe});
                for (const auto& [nbr, e] : tree[n]) {
                    if (!visited[nbr]) {
                        visited[nbr] = 1;
                        stack.push_back({nbr, n, e});
                    }
                }
            }
            for (std::size_t i = post.size(); i-- > 1;) {
                const auto [n, par, pe] = post[i];
                if (odd[n]) {
                    flipEdge(edges[pe]);
                    odd[n] = 0;
                    odd[par] = static_cast<char>(odd[par] ^ 1);
                }
            }
        }
        recomputeB();
        for (std::size_t i = 0; i < nNode; ++i) {
            if (b[i] & 1) {
                r.reason = "parity adjustment failed (odd component)";
                return r;
            }
        }
    }
    // Double cover: v -> v+ (2v), v- (2v+1); every bi-edge becomes two
    // directed edges. Head-head (x,y): (x- -> y+), (y- -> x+); tail-tail:
    // (x+ -> y-), (y+ -> x-). Deviation edges carry convex marginal costs
    // sampled at even offsets (capacity-2 parallel steps).
    detail::MinCostFlow flow;
    const int S = static_cast<int>(2 * nNode);
    const int T = S + 1;
    flow.init(T + 1);
    constexpr int kSteps = 5;
    struct DevEdges {
        std::vector<int> upA, upB, dnA, dnB;  // parallel step edge ids per copy
    };
    std::vector<DevEdges> arcDev(nArc);
    const auto vP = [](long long v) { return static_cast<int>(2 * v); };
    const auto vM = [](long long v) { return static_cast<int>(2 * v + 1); };
    for (std::size_t a = 0; a < nArc; ++a) {
        const long long x = arcNode[a][0], y = arcNode[a][1];
        for (int i = 1; i <= kSteps; ++i) {
            const double m =
                std::max(0.0, (cost(a, g[a] + 2 * i) - cost(a, g[a] + 2 * (i - 1))) * 0.5);
            const int c = static_cast<int>(std::lround(m * kCostScale)) + (i == kSteps ? 1 : 0);
            const int cap = i == kSteps ? (1 << 20) : 2;
            arcDev[a].upA.push_back(flow.addEdge(vM(x), vP(y), cap, c));
            arcDev[a].upB.push_back(flow.addEdge(vM(y), vP(x), cap, c));
        }
        long long slack = g[a] - 1;  // keep x >= 1 (min-one / collapse guard)
        for (int i = 1; i <= kSteps && slack > 0; ++i) {
            const long long lo = std::max<long long>(g[a] - 2 * i, 1);
            const double m = std::max(0.0, (cost(a, lo) - cost(a, std::min(g[a], lo + 2))) * 0.5);
            const int c = static_cast<int>(std::lround(m * kCostScale));
            const int cap = static_cast<int>(std::min<long long>(2, slack));
            slack -= cap;
            arcDev[a].dnA.push_back(flow.addEdge(vP(x), vM(y), cap, c));
            arcDev[a].dnB.push_back(flow.addEdge(vP(y), vM(x), cap, c));
        }
    }
    for (std::size_t ii = 0; ii < nInner; ++ii) {
        const InnerEdge& e = inner[ii];
        const long long x = e.n0, y = e.n1;
        if (e.loop) {
            // Head-head loop at the interior node: up copies raise the loop
            // flow (I- -> I+), down copies lower it, both free — the loop
            // only carries the even-sum pairing, never a deviation cost.
            const int dnCap = static_cast<int>(std::max<long long>(e.g - 1, 0));
            flow.addEdge(vM(x), vP(x), 1 << 20, 0);
            flow.addEdge(vM(x), vP(x), 1 << 20, 0);
            flow.addEdge(vP(x), vM(x), dnCap, 0);
            flow.addEdge(vP(x), vM(x), dnCap, 0);
            continue;
        }
        flow.addEdge(vP(x), vM(y), 1 << 20, 0);
        flow.addEdge(vP(y), vM(x), 1 << 20, 0);
        const int dnCap = static_cast<int>(std::max<long long>(e.g - 1, 0));
        flow.addEdge(vM(x), vP(y), dnCap, 0);
        flow.addEdge(vM(y), vP(x), dnCap, 0);
    }
    long long supply = 0;
    for (std::size_t v = 0; v < nNode; ++v) {
        // b(v+) = b[v], b(v-) = -b[v].
        for (const auto& [node, dem] : {std::make_pair(vP(static_cast<long long>(v)), b[v]),
                                        std::make_pair(vM(static_cast<long long>(v)), -b[v])}) {
            if (dem > 0) {
                flow.addEdge(S, node, static_cast<int>(dem), 0);
                supply += dem;
            } else if (dem < 0) {
                flow.addEdge(node, T, static_cast<int>(-dem), 0);
            }
        }
    }
    const auto [pushed, costTotal] = flow.solve(S, T);
    r.coverCost = costTotal;
    if (pushed < supply) {
        r.reason = "cover flow infeasible (pushed " + std::to_string(pushed) + " of " +
                   std::to_string(supply) + ")";
        return r;
    }
    // Map back: f = g + (up copies)/2 - (down copies)/2.
    r.arcLenHalf.assign(nArc, 0);
    for (std::size_t a = 0; a < nArc; ++a) {
        long long up = 0, dn = 0;
        for (const int e : arcDev[a].upA) {
            up += flow.flow(e);
        }
        for (const int e : arcDev[a].upB) {
            up += flow.flow(e);
        }
        for (const int e : arcDev[a].dnA) {
            dn += flow.flow(e);
        }
        for (const int e : arcDev[a].dnB) {
            dn += flow.flow(e);
        }
        long long x = g[a] + (up - dn + (up - dn >= 0 ? 1 : -1)) / 2;  // round half away
        if ((up - dn) % 2 == 0) {
            x = g[a] + (up - dn) / 2;
        } else {
            ++r.halfIntegral;
        }
        if (x < 1) {
            x = 1;
            ++r.raisedToMin;
        }
        r.arcLenHalf[a] = x;
    }
    // Consistency audit: opposite side sums for quads (exact when the
    // mapped-back flow was integral everywhere); even boundary sum for
    // polygonal patches (odd = unquantizable under the template).
    for (std::size_t p = 0; p < nPatch; ++p) {
        const Patch& patch = tm.patches[p];
        if (patch.isQuad()) {
            for (int j = 0; j < 2; ++j) {
                long long s0 = 0, s1 = 0;
                for (const std::size_t a : patch.side[static_cast<std::size_t>(j)]) {
                    s0 += r.arcLenHalf[a];
                }
                for (const std::size_t a : patch.side[static_cast<std::size_t>(j + 2)]) {
                    s1 += r.arcLenHalf[a];
                }
                r.maxSideViolation = std::max(r.maxSideViolation, std::llabs(s0 - s1));
            }
        } else {
            long long boundary = 0;
            for (const auto& side : patch.side) {
                for (const std::size_t a : side) {
                    boundary += r.arcLenHalf[a];
                }
            }
            if (boundary % 2 != 0) {
                ++r.polyOddSum;
            }
        }
    }
    for (std::size_t a = 0; a < nArc; ++a) {
        r.deviationEnergy += std::abs(0.5 * static_cast<double>(r.arcLenHalf[a]) - tm.arcs[a].len) /
                             std::max(tm.arcs[a].len, 0.5);
    }
    r.ok = true;
    return r;
}

}  // namespace cyber::remesh::bimdf
