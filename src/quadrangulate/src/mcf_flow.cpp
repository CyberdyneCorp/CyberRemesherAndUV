#include <algorithm>
#include <array>
#include <cstdlib>
#include <random>
#include <unordered_set>
#include <vector>

#include "cyber/quadrangulate/mcf_layout.hpp"
#include "mcf_detail.hpp"

// M3b of docs/mcf-integer-layout-plan.md: the flow setup — the tail of QuadriFlow's
// BuildIntegerConstraints (parametrizer-int.cpp). Builds the per-variable table and
// the deterministic "full-flow" pre-adjustment of edge_diff so that each component's
// residual can be fully cancelled by the M3c max-flow. Pure integer math — no SciPP.
namespace cyber::remesh {

McfFlowSetup buildMcfFlowSetup(const Mesh& mesh, const McfEdgeInfo& info,
                               const McfConstraints& con) {
    using mcf::rshift90;
    using mcf::uz;
    McfFlowSetup out;
    if (!info.valid || !con.valid) {
        return out;
    }
    const int m = static_cast<int>(info.faceList.size());
    const int nE = static_cast<int>(info.edgeDiff.size());

    // Working copy of the layout + a mutable residual we drive toward reducibility.
    out.edgeDiff = info.edgeDiff;
    out.totalFlow = con.totalFlow;

    // Sharp (feature) edges + the vertices they touch.
    std::vector<char> sharpUE(uz(nE), 0);
    std::unordered_set<Index> sharpVert;
    for (int e = 0; e < nE; ++e) {
        const auto [a, b] = info.edgeValues[uz(e)];
        const EdgeId me = mesh.edgeBetween(VertexId{a}, VertexId{b});
        if (me.valid() && mesh.isFeatureEdge(me)) {
            sharpUE[uz(e)] = 1;
            sharpVert.insert(a);
            sharpVert.insert(b);
        }
    }

    // allow_changes: a variable is fixed when it is a zero-component of a sharp edge
    // whose both endpoints are sharp (parametrizer.cpp). No features -> all free.
    out.allowChanges.assign(uz(nE) * 2, 1);
    for (int i = 0; i < m * 3; ++i) {
        const int e = info.faceEdgeIds[uz(i / 3)][uz(i % 3)];
        if (!sharpUE[uz(e)]) {
            continue;
        }
        const auto [a, b] = info.edgeValues[uz(e)];
        if (sharpVert.count(a) && sharpVert.count(b)) {
            if (out.edgeDiff[uz(e)].x == 0) {
                out.allowChanges[uz(e * 2)] = 0;
            }
            if (out.edgeDiff[uz(e)].y == 0) {
                out.allowChanges[uz(e * 2 + 1)] = 0;
            }
        }
    }

    // variables[v] = ({the up-to-two face slots referencing scalar var v}, net sign).
    out.variable.assign(uz(nE) * 2, {std::array<int, 2>{-1, -1}, 0});
    const auto comp = [](Vec2i v, int k) { return k == 0 ? v.x : v.y; };
    for (int i = 0; i < m; ++i) {
        for (int j = 0; j < 3; ++j) {
            const int orient = con.faceEdgeOrients[uz(i)][uz(j)];
            const Vec2i sign = rshift90({1, 1}, orient);
            const int eid = info.faceEdgeIds[uz(i)][uz(j)];
            const Vec2i index = rshift90({eid * 2, eid * 2 + 1}, orient);
            for (int k = 0; k < 2; ++k) {
                const int idx = std::abs(comp(index, k));
                auto& p = out.variable[uz(idx)];
                if (p.first[0] == -1) {
                    p.first[0] = i * 2 + k;
                } else {
                    p.first[1] = i * 2 + k;
                }
                p.second += comp(sign, k);
            }
        }
    }

    // Candidate variables to pre-adjust so each component reaches full flow
    // (parametrizer-int.cpp). modified[step][component] = (variable id, +-1 change).
    std::vector<std::vector<std::pair<int, int>>> modified[2];
    for (int s = 0; s < 2; ++s) {
        modified[s].resize(out.totalFlow.size());
    }
    const int nV = static_cast<int>(out.variable.size());
    for (int v = 0; v < nV; ++v) {
        if (!((out.variable[uz(v)].first[1] == -1 || out.variable[uz(v)].second != 0) &&
              out.allowChanges[uz(v)] == 1)) {
            continue;
        }
        const int find = con.sharpColor[uz(out.variable[uz(v)].first[0] / 2)];
        const int step = std::abs(out.variable[uz(v)].second) % 2;
        const int sec = out.variable[uz(v)].second;
        const int d = (v % 2 == 0) ? out.edgeDiff[uz(v / 2)].x : out.edgeDiff[uz(v / 2)].y;
        if (out.totalFlow[uz(find)] > 0) {
            if (sec > 0 && d > -1) {
                modified[uz(step)][uz(find)].push_back({v, -1});
            }
            if (sec < 0 && d < 1) {
                modified[uz(step)][uz(find)].push_back({v, 1});
            }
        } else if (out.totalFlow[uz(find)] < 0) {
            if (sec < 0 && d > -1) {
                modified[uz(step)][uz(find)].push_back({v, -1});
            }
            if (sec > 0 && d < 1) {
                modified[uz(step)][uz(find)].push_back({v, 1});
            }
        }
    }

    // Deterministic shuffle (QuadriFlow seeds rng_seed = 0) then apply.
    std::mt19937 g(0);
    for (int s = 0; s < 2; ++s) {
        for (auto& mv : modified[s]) {
            std::shuffle(mv.begin(), mv.end(), g);
        }
    }
    for (int j = 0; j < static_cast<int>(out.totalFlow.size()); ++j) {
        for (int ii = 0; ii < 2; ++ii) {
            if (out.totalFlow[uz(j)] == 0) {
                continue;
            }
            const int avail = static_cast<int>(modified[uz(ii)][uz(j)].size());
            const int want =
                (ii == 0) ? std::abs(out.totalFlow[uz(j)]) / 2 : std::abs(out.totalFlow[uz(j)]);
            const int maxNum = std::min(want, avail);
            const int dir = (out.totalFlow[uz(j)] > 0) ? -1 : 1;
            for (int i = 0; i < maxNum; ++i) {
                const auto& mvi = modified[uz(ii)][uz(j)][uz(i)];
                if (mvi.first % 2 == 0) {
                    out.edgeDiff[uz(mvi.first / 2)].x += mvi.second;
                } else {
                    out.edgeDiff[uz(mvi.first / 2)].y += mvi.second;
                }
                out.totalFlow[uz(j)] += (ii == 0) ? 2 * dir : dir;
            }
        }
    }

    out.valid = true;
    return out;
}

}  // namespace cyber::remesh
