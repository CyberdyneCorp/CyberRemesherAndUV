#include "sparse_cholesky.hpp"

#include <algorithm>
#include <cmath>
#include <cstdint>
#include <limits>
#include <queue>

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
    m_order = reverseCuthillMcKee(n, adjStart, adj);
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
