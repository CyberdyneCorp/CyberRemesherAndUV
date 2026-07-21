#include "cyber/quadrangulate/mcf_layout.hpp"

#include <algorithm>
#include <array>
#include <cstdint>
#include <cstdlib>
#include <unordered_map>
#include <vector>

#include "mcf_detail.hpp"

#ifdef CYBER_HAVE_SCIPP
#include "numpp/numpp.hpp"
#include "scipp/scipp.hpp"
#endif

namespace cyber::remesh {

bool mcfMaxFlowSelfTest() {
#ifdef CYBER_HAVE_SCIPP
    using namespace numpp;
    using scipp::sparse::CooMatrix;
    using scipp::sparse::CsrMatrix;

    // Directed capacity network, source=0 sink=3, known max-flow = 5. This mirrors
    // the standalone M0 smoke test and confirms the exact primitive QuadriFlow uses
    // (scipp::sparse::csgraph::maximum_flow) is linked and correct in-tree.
    const std::int64_t n = 4;
    const int fr[5] = {0, 0, 1, 1, 2};
    const int to[5] = {1, 2, 2, 3, 3};
    const double cap[5] = {3, 2, 2, 2, 3};
    ndarray data = zeros({5}, kFloat64);
    ndarray row = zeros({5}, kInt64);
    ndarray col = zeros({5}, kInt64);
    for (int i = 0; i < 5; ++i) {
        data.typed_data<double>()[i] = cap[i];
        row.typed_data<std::int64_t>()[i] = fr[i];
        col.typed_data<std::int64_t>()[i] = to[i];
    }
    CooMatrix coo;
    coo.data = data;
    coo.row = row;
    coo.col = col;
    coo.rows = n;
    coo.cols = n;
    const CsrMatrix g = CsrMatrix::from_coo(coo);
    return scipp::sparse::csgraph::maximum_flow(g, 0, 3).flow_value == 5;
#else
    return false;
#endif
}

McfSolveResult solveMcfFlow(const McfEdgeInfo& info, const McfConstraints& con,
                            const McfFlowSetup& setup) {
#ifdef CYBER_HAVE_SCIPP
    using mcf::rshift90;
    using mcf::uz;
    using numpp::kFloat64;
    using numpp::kInt64;
    using numpp::ndarray;
    using numpp::zeros;
    using scipp::sparse::CooMatrix;
    using scipp::sparse::CsrMatrix;

    McfSolveResult res;
    if (!info.valid || !con.valid || !setup.valid) {
        return res;
    }
    const int m = static_cast<int>(info.faceList.size());
    const int nE = static_cast<int>(info.edgeDiff.size());
    const int nEq = m * 2;
    std::vector<Vec2i> edgeDiff = setup.edgeDiff;
    const auto comp = [](Vec2i v, int k) { return k == 0 ? v.x : v.y; };
    const auto addComp = [](Vec2i& v, int k, int d) { (k == 0 ? v.x : v.y) += d; };

    int edgeCapacity = 2;
    bool fullFlow = false;
    for (int iter = 0; iter < 10 && !fullFlow; ++iter) {
        // Per-variable equation map + per-equation residual (initial).
        std::vector<std::array<int, 4>> etc(uz(nE * 2), {-1, 0, -1, 0});
        std::vector<int> initial(uz(nEq), 0);
        for (int i = 0; i < m; ++i) {
            for (int j = 0; j < 3; ++j) {
                const int e = info.faceEdgeIds[uz(i)][uz(j)];
                const Vec2i index = rshift90({e * 2 + 1, e * 2 + 2}, con.faceEdgeOrients[uz(i)][uz(j)]);
                for (int k = 0; k < 2; ++k) {
                    const int idx = comp(index, k);
                    const int l = std::abs(idx), s = idx / l, ind = l - 1, eq = i * 2 + k;
                    if (etc[uz(ind)][0] == -1) {
                        etc[uz(ind)][0] = eq;
                        etc[uz(ind)][1] = s;
                    } else {
                        etc[uz(ind)][2] = eq;
                        etc[uz(ind)][3] = s;
                    }
                    initial[uz(eq)] += s * comp(edgeDiff[uz(ind / 2)], ind % 2);
                }
            }
        }

        // Flow network. Nodes: 0 = source, 1..nEq = equations (eq -> eq+1),
        // nEq+1 = sink, then a unique middle node per regular-arc direction (so
        // parallel arcs stay distinct in the CSR, which sums duplicate entries).
        const int kSrc = 0, kSink = nEq + 1;
        int nextNode = nEq + 2;
        std::vector<double> cooData;
        std::vector<std::int64_t> cooRow, cooCol;
        const auto add = [&](int u, int v, int c) {
            if (c <= 0) {
                return;
            }
            cooData.push_back(static_cast<double>(c));
            cooRow.push_back(u);
            cooCol.push_back(v);
        };
        struct ArcRec {
            int var, fwdU, fwdMid, bwdU, bwdMid;
        };
        std::vector<ArcRec> arcRecs;
        for (int i = 0; i < nE * 2; ++i) {
            if (setup.allowChanges[uz(i)] == 0 || etc[uz(i)][0] == -1 || etc[uz(i)][2] == -1) {
                continue;
            }
            if (etc[uz(i)][1] != -etc[uz(i)][3]) {
                continue;
            }
            int v1 = etc[uz(i)][0], v2 = etc[uz(i)][2];
            if (etc[uz(i)][1] < 0) {
                std::swap(v1, v2);
            }
            const int c = comp(edgeDiff[uz(i / 2)], i % 2);
            const int fwd = std::max(0, c + edgeCapacity), bwd = std::max(0, -c + edgeCapacity);
            const int u1 = v1 + 1, u2 = v2 + 1, mf = nextNode++, mb = nextNode++;
            add(u1, mf, fwd);
            add(mf, u2, fwd);
            add(u2, mb, bwd);
            add(mb, u1, bwd);
            arcRecs.push_back({i, u1, mf, u2, mb});
        }
        int supply = 0;
        for (int eq = 0; eq < nEq; ++eq) {
            if (initial[uz(eq)] > 0) {
                add(kSrc, eq + 1, initial[uz(eq)]);
                supply += initial[uz(eq)];
            } else if (initial[uz(eq)] < 0) {
                add(eq + 1, kSink, -initial[uz(eq)]);
            }
        }
        res.supply = supply;

        const int nNodes = nextNode;
        const int nnz = static_cast<int>(cooData.size());
        ndarray data = zeros({nnz}, kFloat64), row = zeros({nnz}, kInt64), col = zeros({nnz}, kInt64);
        for (int i = 0; i < nnz; ++i) {
            data.typed_data<double>()[i] = cooData[uz(i)];
            row.typed_data<std::int64_t>()[i] = cooRow[uz(i)];
            col.typed_data<std::int64_t>()[i] = cooCol[uz(i)];
        }
        CooMatrix coo;
        coo.data = data;
        coo.row = row;
        coo.col = col;
        coo.rows = nNodes;
        coo.cols = nNodes;
        const CsrMatrix g = CsrMatrix::from_coo(coo);
        const auto r = scipp::sparse::csgraph::maximum_flow(g, kSrc, kSink);
        res.flow = static_cast<int>(r.flow_value);

        // Read the per-edge flow: map (u,v) -> flow from the result CSR.
        std::unordered_map<std::int64_t, int> flowMap;
        const CsrMatrix& fm = r.flow;
        const std::int64_t* fIndptr = fm.indptr().typed_data<std::int64_t>();
        const std::int64_t* fIndices = fm.indices().typed_data<std::int64_t>();
        const double* fData = fm.data().typed_data<double>();
        for (std::int64_t u = 0; u < fm.rows(); ++u) {
            for (std::int64_t p = fIndptr[u]; p < fIndptr[u + 1]; ++p) {
                const int f = static_cast<int>(fData[p]);
                if (f != 0) {
                    flowMap[u * nNodes + fIndices[p]] = f;
                }
            }
        }
        const auto flowOf = [&](int u, int v) {
            const auto it = flowMap.find(static_cast<std::int64_t>(u) * nNodes + v);
            return it == flowMap.end() ? 0 : it->second;
        };
        // applyTo: forward flow decreases the variable, backward flow increases it.
        for (const ArcRec& a : arcRecs) {
            const int net = -flowOf(a.fwdU, a.fwdMid) + flowOf(a.bwdU, a.bwdMid);
            if (net != 0) {
                addComp(edgeDiff[uz(a.var / 2)], a.var % 2, net);
            }
        }

        if (res.flow == supply) {
            fullFlow = true;
        } else {
            ++edgeCapacity;
        }
    }

    res.edgeDiff = std::move(edgeDiff);
    res.fullFlow = fullFlow;
    res.valid = true;
    return res;
#else
    (void)info;
    (void)con;
    (void)setup;
    return {};
#endif
}

}  // namespace cyber::remesh
