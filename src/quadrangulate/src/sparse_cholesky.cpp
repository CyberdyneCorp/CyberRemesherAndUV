#include "sparse_cholesky.hpp"

#include <algorithm>
#include <cmath>
#include <cstdint>
#include <cstdio>
#include <cstdlib>
#include <limits>
#include <queue>
#include <string>

namespace cyber::remesh {

namespace {

constexpr std::size_t kNone = std::numeric_limits<std::size_t>::max();

// Reverse Cuthill-McKee ordering of the symmetric pattern (diagonal ignored).
// Per connected component: pick a pseudo-peripheral start (repeated BFS toward
// the farthest, lowest-degree node), then breadth-first visit with neighbors in
// ascending-degree order; the concatenated order is reversed at the end. Keeps
// the factor banded on mesh-like graphs, which is what bounds the LL^T fill.
std::vector<std::size_t> reverseCuthillMcKee(std::size_t n,
                                             const std::vector<std::size_t>& adjStart,
                                             const std::vector<std::size_t>& adj) {
    const auto degree = [&](std::size_t v) { return adjStart[v + 1] - adjStart[v]; };
    std::vector<std::size_t> order;
    order.reserve(n);
    std::vector<char> visited(n, 0);
    std::vector<std::size_t> level(n, 0);
    std::vector<std::size_t> component;

    // BFS from `start` over unvisited-in-this-search vertices; returns the
    // vertices reached in visit order and fills `level` with BFS depths.
    std::vector<char> seen(n, 0);
    const auto bfs = [&](std::size_t start) {
        component.clear();
        std::queue<std::size_t> q;
        q.push(start);
        seen[start] = 1;
        level[start] = 0;
        while (!q.empty()) {
            const std::size_t u = q.front();
            q.pop();
            component.push_back(u);
            for (std::size_t p = adjStart[u]; p < adjStart[u + 1]; ++p) {
                const std::size_t w = adj[p];
                if (!seen[w] && !visited[w]) {
                    seen[w] = 1;
                    level[w] = level[u] + 1;
                    q.push(w);
                }
            }
        }
        for (const std::size_t u : component) {
            seen[u] = 0;  // reset for the next search
        }
    };

    for (std::size_t seed = 0; seed < n; ++seed) {
        if (visited[seed]) {
            continue;
        }
        // Pseudo-peripheral vertex: walk to the farthest BFS level a few times.
        std::size_t start = seed;
        std::size_t ecc = 0;
        for (int iter = 0; iter < 4; ++iter) {
            bfs(start);
            std::size_t far = start;
            std::size_t farLevel = 0;
            for (const std::size_t u : component) {
                if (level[u] > farLevel || (level[u] == farLevel && degree(u) < degree(far))) {
                    farLevel = level[u];
                    far = u;
                }
            }
            if (farLevel <= ecc) {
                break;
            }
            ecc = farLevel;
            start = far;
        }
        // Cuthill-McKee visit from `start`, neighbors in ascending degree.
        std::vector<std::size_t> queue{start};
        visited[start] = 1;
        std::size_t head = 0;
        std::vector<std::size_t> nbrs;
        while (head < queue.size()) {
            const std::size_t u = queue[head++];
            order.push_back(u);
            nbrs.clear();
            for (std::size_t p = adjStart[u]; p < adjStart[u + 1]; ++p) {
                if (!visited[adj[p]]) {
                    visited[adj[p]] = 1;
                    nbrs.push_back(adj[p]);
                }
            }
            std::sort(nbrs.begin(), nbrs.end(), [&](std::size_t a, std::size_t b) {
                const std::size_t da = degree(a), db = degree(b);
                return da != db ? da < db : a < b;
            });
            queue.insert(queue.end(), nbrs.begin(), nbrs.end());
        }
    }
    std::reverse(order.begin(), order.end());
    return order;
}

// Approximate minimum degree ordering (Amestoy-Davis-Duff style) with
// aggressive element absorption, mass elimination, and hash-based
// supervariable merging — implemented from the published algorithm on the same
// deduplicated off-diagonal adjacency RCM uses (dependency-free). The quotient
// graph keeps each node's neighbor list in one arena: a live variable stores
// [adjacent elements | adjacent variables], a live element stores its member
// variables. Lists only shrink in place; each pivot's element list is appended
// to the arena, so no garbage collection is needed (total growth is bounded by
// the factor's row patterns, small next to the factor itself).
std::vector<std::size_t> approximateMinimumDegree(std::size_t nIn,
                                                  const std::vector<std::size_t>& adjStart,
                                                  const std::vector<std::size_t>& adj) {
    const int n = static_cast<int>(nIn);
    if (n == 0) {
        return {};
    }
    std::vector<int> mem(adj.begin(), adj.end());
    std::vector<int> startV(nIn), lenV(nIn);
    std::vector<int> elenV(nIn, 0);  // >= 0: live variable (element count); -1: element; -2: dead
    std::vector<int> nvV(nIn, 1);    // supervariable weight (0 once merged/eliminated)
    std::vector<int> degV(nIn);      // variable: approx external degree; element: |Le| at creation
    std::vector<std::uint64_t> wV(nIn, 0);    // pass-1 per-element |Le \ Lp| workspace
    std::vector<int> inLpV(nIn, -1);          // pivot-stamped Lp membership
    std::vector<std::int64_t> cmpV(nIn, -1);  // stamp for supervariable list comparison
    std::vector<int> dheadV(nIn, -1), dnextV(nIn, -1), dprevV(nIn, -1);  // degree lists
    std::vector<int> hheadV(nIn, -1), hnextV(nIn, -1);                   // supervariable hash
    // Chain of variables merged into a principal, emitted right after it.
    std::vector<int> chainNextV(nIn, -1), chainTailV(nIn);

    // Signed-index aliases (int subscripts on raw pointers keep -Wsign-conversion
    // quiet without a cast at every access). `mem` grows, so memp is refreshed
    // after every append.
    int* memp = mem.data();
    int* const start = startV.data();
    int* const len = lenV.data();
    int* const elen = elenV.data();
    int* const nv = nvV.data();
    int* const deg = degV.data();
    std::uint64_t* const w = wV.data();
    int* const inLp = inLpV.data();
    std::int64_t* const cmp = cmpV.data();
    int* const dhead = dheadV.data();
    int* const dnext = dnextV.data();
    int* const dprev = dprevV.data();
    int* const hhead = hheadV.data();
    int* const hnext = hnextV.data();
    int* const chainNext = chainNextV.data();
    int* const chainTail = chainTailV.data();

    const auto listInsert = [&](int i, int d) {
        dprev[i] = -1;
        dnext[i] = dhead[d];
        if (dhead[d] >= 0) {
            dprev[dhead[d]] = i;
        }
        dhead[d] = i;
    };
    const auto listRemove = [&](int i, int d) {
        if (dprev[i] >= 0) {
            dnext[dprev[i]] = dnext[i];
        } else {
            dhead[d] = dnext[i];
        }
        if (dnext[i] >= 0) {
            dprev[dnext[i]] = dprev[i];
        }
    };

    for (int i = 0; i < n; ++i) {
        const auto ui = static_cast<std::size_t>(i);
        start[i] = static_cast<int>(adjStart[ui]);
        len[i] = static_cast<int>(adjStart[ui + 1]) - start[i];
        deg[i] = len[i];
        chainTail[i] = i;
        listInsert(i, deg[i]);
    }

    std::vector<int> pivotSeq;
    pivotSeq.reserve(nIn);
    std::vector<int> lp, keepElem, keepVar, touched;
    std::uint64_t wflg = 1;
    std::int64_t cmpTag = 0;
    int eliminated = 0;
    int minDeg = 0;
    int pivot = 0;

    while (eliminated < n) {
        while (dhead[minDeg] < 0) {
            ++minDeg;
        }
        const int p = dhead[minDeg];
        listRemove(p, minDeg);
        ++pivot;

        // Gather Lp = live variables adjacent to p directly or via p's
        // elements; those elements are absorbed into the new element p.
        lp.clear();
        inLp[p] = pivot;
        int degme = 0;
        const auto addVar = [&](int j) {
            if (elen[j] < 0 || nv[j] <= 0 || inLp[j] == pivot) {
                return;
            }
            inLp[j] = pivot;
            listRemove(j, deg[j]);
            degme += nv[j];
            lp.push_back(j);
        };
        for (int q = start[p]; q < start[p] + elen[p]; ++q) {
            const int e = memp[q];
            if (elen[e] != -1) {
                continue;
            }
            for (int r = start[e]; r < start[e] + len[e]; ++r) {
                addVar(memp[r]);
            }
            elen[e] = -2;  // absorbed into element p
        }
        for (int q = start[p] + elen[p]; q < start[p] + len[p]; ++q) {
            addVar(memp[q]);
        }
        const int nvp = nv[p];
        start[p] = static_cast<int>(mem.size());
        mem.insert(mem.end(), lp.begin(), lp.end());
        memp = mem.data();
        len[p] = static_cast<int>(lp.size());
        elen[p] = -1;  // p is now an element

        // Pass 1: w[e] - wflg = |Le \ Lp| (weighted, approximate via deg[e])
        // for every live element touching Lp.
        wflg += static_cast<std::uint64_t>(n) + 1;
        for (const int i : lp) {
            for (int q = start[i]; q < start[i] + elen[i]; ++q) {
                const int e = memp[q];
                if (elen[e] != -1) {
                    continue;
                }
                if (w[e] < wflg) {
                    w[e] = wflg + static_cast<std::uint64_t>(deg[e]);
                }
                w[e] -= static_cast<std::uint64_t>(nv[i]);
            }
        }

        // Pass 2: prune each i's lists (drop dead entries, absorbed elements,
        // and Lp variables now covered by element p), add element p, compute
        // the approximate external degree, mass-eliminate d == 0 variables,
        // and hash the pruned list for supervariable detection.
        touched.clear();
        for (const int i : lp) {
            keepElem.clear();
            keepVar.clear();
            std::uint64_t hash = 0;
            std::int64_t d = 0;
            for (int q = start[i]; q < start[i] + elen[i]; ++q) {
                const int e = memp[q];
                if (elen[e] != -1) {
                    continue;
                }
                const std::int64_t dext =
                    w[e] >= wflg ? static_cast<std::int64_t>(w[e] - wflg) : deg[e];
                if (dext > 0) {
                    d += dext;
                    keepElem.push_back(e);
                    hash += static_cast<std::uint64_t>(e);
                } else {
                    elen[e] = -2;  // aggressive absorption: Le is inside Lp
                }
            }
            for (int q = start[i] + elen[i]; q < start[i] + len[i]; ++q) {
                const int j = memp[q];
                if (elen[j] < 0 || nv[j] <= 0 || inLp[j] == pivot) {
                    continue;
                }
                d += nv[j];
                keepVar.push_back(j);
                hash += static_cast<std::uint64_t>(j);
            }
            if (d == 0) {
                // Mass elimination: i's neighbors all lie in Lp ∪ {p}, so
                // eliminating i right after p adds no fill.
                elen[i] = -2;
                chainNext[chainTail[p]] = i;
                chainTail[p] = chainTail[i];
                eliminated += nv[i];
                degme -= nv[i];
                nv[i] = 0;
                continue;
            }
            deg[i] = static_cast<int>(d);  // raw external degree, finalized below
            // Rewrite in place: [kept elements | p | kept variables]. This
            // always fits: i was reached through p's variable list (variable p
            // was dropped) or through an absorbed element (dropped above).
            int q = start[i];
            for (const int e : keepElem) {
                memp[q++] = e;
            }
            memp[q++] = p;
            elen[i] = static_cast<int>(keepElem.size()) + 1;
            for (const int j : keepVar) {
                memp[q++] = j;
            }
            len[i] = q - start[i];
            const int bucket = static_cast<int>(hash % static_cast<std::uint64_t>(n));
            if (hhead[bucket] < 0) {
                touched.push_back(bucket);
            }
            hnext[i] = hhead[bucket];
            hhead[bucket] = i;
        }

        // Supervariable detection: merge variables with identical pruned lists
        // (they will share a factor column pattern) so later degree updates
        // treat them as one.
        for (const int bucket : touched) {
            for (int i = hhead[bucket]; i >= 0; i = hnext[i]) {
                if (nv[i] <= 0 || hnext[i] < 0) {
                    continue;
                }
                ++cmpTag;
                for (int q = start[i]; q < start[i] + len[i]; ++q) {
                    cmp[memp[q]] = cmpTag;
                }
                int prev = i;
                for (int j = hnext[i]; j >= 0; j = hnext[prev]) {
                    bool same = nv[j] > 0 && len[j] == len[i] && elen[j] == elen[i];
                    for (int q = start[j]; same && q < start[j] + len[j]; ++q) {
                        same = cmp[memp[q]] == cmpTag;
                    }
                    if (same) {
                        nv[i] += nv[j];
                        nv[j] = 0;
                        elen[j] = -2;
                        chainNext[chainTail[i]] = j;
                        chainTail[i] = chainTail[j];
                        hnext[prev] = hnext[j];
                    } else {
                        prev = j;
                    }
                }
            }
            hhead[bucket] = -1;
        }

        // Re-list the surviving Lp variables with their new approximate degree
        // d(external) + |Lp \ i|, capped by the live variable count.
        for (const int i : lp) {
            if (elen[i] < 0 || nv[i] <= 0) {
                continue;
            }
            const int cap = n - eliminated - nv[i];
            const int d = std::min(deg[i] + degme - nv[i], cap);
            deg[i] = d;
            listInsert(i, d);
            minDeg = std::min(minDeg, d);
        }
        deg[p] = degme;
        if (len[p] == 0) {
            elen[p] = -2;  // empty element: nothing references it
        }
        eliminated += nvp;
        pivotSeq.push_back(p);
    }

    // Elimination order, each principal followed by everything merged into it.
    std::vector<std::size_t> order;
    order.reserve(nIn);
    for (const int p : pivotSeq) {
        for (int v = p; v >= 0; v = chainNext[v]) {
            order.push_back(static_cast<std::size_t>(v));
        }
    }
    return order;
}

// Strictly-lower nonzero count of the LL^T factor that `order` yields on the
// adjacency pattern (elimination tree + row-pattern walk, no numerics). Used
// to pick the cheaper of RCM/AMD before committing to a factorization.
std::size_t symbolicFill(std::size_t n, const std::vector<std::size_t>& adjStart,
                         const std::vector<std::size_t>& adj,
                         const std::vector<std::size_t>& order) {
    std::vector<std::size_t> rank(n);
    for (std::size_t k = 0; k < n; ++k) {
        rank[order[k]] = k;
    }
    // Permuted strictly-lower pattern rows.
    std::vector<std::size_t> bStart(n + 1, 0);
    for (std::size_t i = 0; i < n; ++i) {
        for (std::size_t p = adjStart[i]; p < adjStart[i + 1]; ++p) {
            if (rank[adj[p]] < rank[i]) {
                ++bStart[rank[i] + 1];
            }
        }
    }
    for (std::size_t k = 0; k < n; ++k) {
        bStart[k + 1] += bStart[k];
    }
    std::vector<std::size_t> bCol(bStart[n]);
    {
        std::vector<std::size_t> next(bStart.begin(), bStart.end() - 1);
        for (std::size_t i = 0; i < n; ++i) {
            for (std::size_t p = adjStart[i]; p < adjStart[i + 1]; ++p) {
                if (rank[adj[p]] < rank[i]) {
                    bCol[next[rank[i]]++] = rank[adj[p]];
                }
            }
        }
    }
    // Elimination tree with path compression, counting row-pattern nodes.
    std::vector<std::size_t> parent(n, kNone), ancestor(n, kNone), mark(n, kNone);
    std::size_t fill = 0;
    for (std::size_t k = 0; k < n; ++k) {
        mark[k] = k;
        for (std::size_t p = bStart[k]; p < bStart[k + 1]; ++p) {
            std::size_t i = bCol[p];
            while (i != kNone && i < k) {
                const std::size_t nextI = ancestor[i];
                ancestor[i] = k;
                if (nextI == kNone) {
                    parent[i] = k;
                }
                i = nextI;
            }
            for (i = bCol[p]; mark[i] != k; i = parent[i]) {
                mark[i] = k;
                ++fill;
            }
        }
    }
    return fill;
}

}  // namespace

bool SparseCholesky::factor(std::size_t n, const std::vector<std::size_t>& rowStart,
                            const std::vector<std::size_t>& colIndex,
                            const std::vector<double>& value, double ridge) {
    m_ready = false;
    m_n = n;
    if (n == 0 || rowStart.size() != n + 1) {
        return false;
    }

    // Deduplicated off-diagonal adjacency for the ordering.
    std::vector<std::size_t> adjStart(n + 1, 0);
    std::vector<std::size_t> adj;
    adj.reserve(colIndex.size());
    {
        std::vector<std::size_t> cols;
        for (std::size_t i = 0; i < n; ++i) {
            cols.assign(colIndex.begin() + static_cast<std::ptrdiff_t>(rowStart[i]),
                        colIndex.begin() + static_cast<std::ptrdiff_t>(rowStart[i + 1]));
            std::sort(cols.begin(), cols.end());
            cols.erase(std::unique(cols.begin(), cols.end()), cols.end());
            for (const std::size_t j : cols) {
                if (j != i && j < n) {
                    adj.push_back(j);
                }
            }
            adjStart[i + 1] = adj.size();
        }
    }
    // Fill-reducing ordering: AMD wins on the solver's operators corpus-wide
    // (2-4x lower fill than RCM on the reduced operator), but the default
    // still measures both symbolically and keeps whichever fills less —
    // ordering quality is graph-dependent and the symbolic count is cheap
    // next to the numeric factorization. CYBER_QC_ORDERING=rcm|amd forces one.
    const char* orderingEnv = std::getenv("CYBER_QC_ORDERING");
    const std::string mode = orderingEnv != nullptr ? orderingEnv : "";
    if (mode == "rcm") {
        m_order = reverseCuthillMcKee(n, adjStart, adj);
    } else if (mode == "amd") {
        m_order = approximateMinimumDegree(n, adjStart, adj);
    } else {
        std::vector<std::size_t> rcm = reverseCuthillMcKee(n, adjStart, adj);
        std::vector<std::size_t> amd = approximateMinimumDegree(n, adjStart, adj);
        const std::size_t fillRcm = symbolicFill(n, adjStart, adj, rcm);
        const std::size_t fillAmd = symbolicFill(n, adjStart, adj, amd);
        if (std::getenv("CYBER_QC_TIME") != nullptr) {
            std::fprintf(stderr, "[qc-time] cholesky ordering: n=%zu fill rcm=%zu amd=%zu -> %s\n",
                         n, fillRcm, fillAmd, fillAmd <= fillRcm ? "amd" : "rcm");
        }
        m_order = fillAmd <= fillRcm ? std::move(amd) : std::move(rcm);
    }
    m_rank.assign(n, 0);
    for (std::size_t k = 0; k < n; ++k) {
        m_rank[m_order[k]] = k;
    }

    // Permuted LOWER triangle B(k, j<=k) in CSR: row k lists (permuted column,
    // value) pairs with duplicate coordinates summed by the scatter later.
    std::vector<std::size_t> bStart(n + 1, 0);
    for (std::size_t i = 0; i < n; ++i) {
        const std::size_t k = m_rank[i];
        for (std::size_t p = rowStart[i]; p < rowStart[i + 1]; ++p) {
            if (colIndex[p] < n && m_rank[colIndex[p]] <= k) {
                ++bStart[k + 1];
            }
        }
    }
    for (std::size_t k = 0; k < n; ++k) {
        bStart[k + 1] += bStart[k];
    }
    std::vector<std::size_t> bCol(bStart[n]);
    std::vector<double> bVal(bStart[n]);
    {
        std::vector<std::size_t> next(bStart.begin(), bStart.end() - 1);
        for (std::size_t i = 0; i < n; ++i) {
            const std::size_t k = m_rank[i];
            for (std::size_t p = rowStart[i]; p < rowStart[i + 1]; ++p) {
                if (colIndex[p] >= n) {
                    continue;
                }
                const std::size_t j = m_rank[colIndex[p]];
                if (j <= k) {
                    bCol[next[k]] = j;
                    bVal[next[k]] = value[p];
                    ++next[k];
                }
            }
        }
    }

    // Elimination tree (parent per column) with path compression.
    std::vector<std::size_t> parent(n, kNone);
    {
        std::vector<std::size_t> ancestor(n, kNone);
        for (std::size_t k = 0; k < n; ++k) {
            for (std::size_t p = bStart[k]; p < bStart[k + 1]; ++p) {
                std::size_t i = bCol[p];
                while (i != kNone && i < k) {
                    const std::size_t nextI = ancestor[i];
                    ancestor[i] = k;
                    if (nextI == kNone) {
                        parent[i] = k;
                    }
                    i = nextI;
                }
            }
        }
    }

    // Row-pattern walk shared by the count and numeric passes: visits the
    // pattern of row k of L in the topological order the up-looking update
    // needs (each column after all its etree descendants in the pattern).
    std::vector<std::size_t> mark(n, kNone);
    std::vector<std::size_t> stack(n), pattern(n);
    const auto rowPattern = [&](std::size_t k) {
        std::size_t top = n;
        mark[k] = k;
        for (std::size_t p = bStart[k]; p < bStart[k + 1]; ++p) {
            std::size_t i = bCol[p];
            if (i >= k) {
                continue;
            }
            std::size_t len = 0;
            while (mark[i] != k) {
                stack[len++] = i;
                mark[i] = k;
                i = parent[i];
            }
            while (len > 0) {
                pattern[--top] = stack[--len];
            }
        }
        return top;
    };

    // Pass 1: column counts of L.
    std::vector<std::size_t> colCount(n, 0);
    for (std::size_t k = 0; k < n; ++k) {
        const std::size_t top = rowPattern(k);
        for (std::size_t p = top; p < n; ++p) {
            ++colCount[pattern[p]];
        }
    }
    m_colStart.assign(n + 1, 0);
    for (std::size_t k = 0; k < n; ++k) {
        m_colStart[k + 1] = m_colStart[k] + colCount[k];
    }
    m_rowIdx.assign(m_colStart[n], 0);
    m_val.assign(m_colStart[n], 0.0);
    m_diag.assign(n, 0.0);

    // Pass 2: numeric up-looking factorization.
    std::fill(mark.begin(), mark.end(), kNone);
    std::vector<std::size_t> colNext(m_colStart.begin(), m_colStart.end() - 1);
    std::vector<double> x(n, 0.0);
    for (std::size_t k = 0; k < n; ++k) {
        const std::size_t top = rowPattern(k);
        double d = ridge;
        for (std::size_t p = bStart[k]; p < bStart[k + 1]; ++p) {
            if (bCol[p] == k) {
                d += bVal[p];
            } else if (bCol[p] < k) {
                x[bCol[p]] += bVal[p];
            }
        }
        for (std::size_t p = top; p < n; ++p) {
            const std::size_t j = pattern[p];
            const double lkj = x[j] / m_diag[j];
            x[j] = 0.0;
            for (std::size_t q = m_colStart[j]; q < colNext[j]; ++q) {
                x[m_rowIdx[q]] -= m_val[q] * lkj;
            }
            d -= lkj * lkj;
            m_rowIdx[colNext[j]] = k;
            m_val[colNext[j]] = lkj;
            ++colNext[j];
        }
        if (!(d > 0.0)) {
            return false;  // lost positivity — caller falls back to CG
        }
        m_diag[k] = std::sqrt(d);
    }
    m_ready = true;
    return true;
}

void SparseCholesky::forwardBackward(std::vector<double>& y, std::size_t firstRow) const {
    const std::size_t n = m_n;
    for (std::size_t j = firstRow; j < n; ++j) {  // L y' = y
        const double yj = y[j] / m_diag[j];
        y[j] = yj;
        if (yj != 0.0) {
            for (std::size_t q = m_colStart[j]; q < m_colStart[j + 1]; ++q) {
                y[m_rowIdx[q]] -= m_val[q] * yj;
            }
        }
    }
    for (std::size_t j = n; j-- > 0;) {  // L^T x = y'
        double t = y[j];
        for (std::size_t q = m_colStart[j]; q < m_colStart[j + 1]; ++q) {
            t -= m_val[q] * y[m_rowIdx[q]];
        }
        y[j] = t / m_diag[j];
    }
}

void SparseCholesky::solve(const std::vector<double>& b, std::vector<double>& x) const {
    std::vector<double> y(m_n);
    for (std::size_t k = 0; k < m_n; ++k) {
        y[k] = b[m_order[k]];
    }
    forwardBackward(y, 0);
    x.resize(m_n);
    for (std::size_t k = 0; k < m_n; ++k) {
        x[m_order[k]] = y[k];
    }
}

void SparseCholesky::solveUnitGather(std::size_t k, const std::vector<std::size_t>& targets,
                                     double* out) const {
    std::vector<double> y(m_n, 0.0);
    const std::size_t pk = m_rank[k];
    y[pk] = 1.0;
    forwardBackward(y, pk);  // rows above pk are exactly zero in the forward pass
    for (std::size_t i = 0; i < targets.size(); ++i) {
        out[i] = y[m_rank[targets[i]]];
    }
}

bool DenseCholeskyInc::add(const std::vector<double>& column, double diagonal) {
    const std::size_t m = m_rows.size();
    std::vector<double> row(m + 1);
    double d = diagonal;
    for (std::size_t i = 0; i < m; ++i) {  // forward solve L l = column
        double t = column[i];
        const std::vector<double>& li = m_rows[i];
        for (std::size_t j = 0; j < i; ++j) {
            t -= li[j] * row[j];
        }
        row[i] = t / li[i];
        d -= row[i] * row[i];
    }
    if (!(d > 0.0)) {
        return false;
    }
    row[m] = std::sqrt(d);
    m_rows.push_back(std::move(row));
    return true;
}

void DenseCholeskyInc::solve(const std::vector<double>& b, std::vector<double>& x) const {
    const std::size_t m = m_rows.size();
    x.assign(m, 0.0);
    for (std::size_t i = 0; i < m; ++i) {
        double t = b[i];
        const std::vector<double>& li = m_rows[i];
        for (std::size_t j = 0; j < i; ++j) {
            t -= li[j] * x[j];
        }
        x[i] = t / li[i];
    }
    for (std::size_t i = m; i-- > 0;) {
        double t = x[i];
        for (std::size_t j = i + 1; j < m; ++j) {
            t -= m_rows[j][i] * x[j];
        }
        x[i] = t / m_rows[i][i];
    }
}

}  // namespace cyber::remesh
