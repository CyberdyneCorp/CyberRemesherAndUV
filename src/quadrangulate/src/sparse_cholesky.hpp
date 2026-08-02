#pragma once

#include <cstddef>
#include <vector>

// In-tree double-precision sparse Cholesky (simplicial LL^T, up-looking, with
// fill-reducing ordering: approximate minimum degree vs reverse Cuthill-McKee,
// picked by measured symbolic fill; CYBER_QC_ORDERING=rcm|amd forces one) plus
// an incrementally growable dense Cholesky. Serves the quad-cover seamless
// solve's direct path (CYBER_QC_DIRECT, docs/ROADMAP.md): the pinned Poisson
// operator and the reduced Dirichlet operator are FIXED across their many
// re-solves, so one factorization + cheap back-substitutions replace thousands
// of CG iterations. Deliberately dependency-free — the quad-cover path must
// stay license-clean (no Eigen/SuiteSparse); the classic textbook algorithms
// (etree + ereach up-looking factorization, BFS-based RCM, quotient-graph AMD)
// are small enough to own.
namespace cyber::remesh {

// Sparse symmetric-positive-definite LL^T factorization of an n x n matrix
// given as full-pattern CSR (both triangles present). `factor` returns false
// (and leaves the object not ready) when the matrix is empty or a pivot loses
// positivity — callers keep their iterative path as the fallback.
class SparseCholesky {
public:
    // `ridge` is added to every diagonal entry before factorization (the
    // reduced operator's well-conditioning ridge; pass 0 for none).
    bool factor(std::size_t n, const std::vector<std::size_t>& rowStart,
                const std::vector<std::size_t>& colIndex, const std::vector<double>& value,
                double ridge = 0.0);

    // Solve A x = b. `b` and `x` have size n (x is overwritten). Thread-safe
    // across concurrent callers (uses only local scratch).
    void solve(const std::vector<double>& b, std::vector<double>& x) const;

    // Solve A x = e_k (column k of A^-1) and gather x at `targets`:
    // out[i] = x[targets[i]]. The forward pass starts at k's permuted position
    // (everything above it is exactly zero). Thread-safe like `solve`.
    void solveUnitGather(std::size_t k, const std::vector<std::size_t>& targets, double* out) const;

    [[nodiscard]] bool ready() const { return m_ready; }
    [[nodiscard]] std::size_t dim() const { return m_n; }
    [[nodiscard]] std::size_t factorNnz() const { return m_rowIdx.size() + m_diag.size(); }

private:
    void forwardBackward(std::vector<double>& y, std::size_t firstRow) const;

    std::size_t m_n = 0;
    bool m_ready = false;
    std::vector<std::size_t> m_order;  // new index -> old index (RCM)
    std::vector<std::size_t> m_rank;   // old index -> new index
    // L (strictly lower triangle) in compressed-column form; unit diagonal is
    // NOT implied — m_diag holds the diagonal of L.
    std::vector<std::size_t> m_colStart;  // size n + 1
    std::vector<std::size_t> m_rowIdx;
    std::vector<double> m_val;
    std::vector<double> m_diag;  // size n
};

// Dense SPD Cholesky that grows one row/column at a time — the bordered
// (Woodbury) system of the greedy integer rounding only ever ADDS pins, so the
// factor is extended in O(m^2) per pin instead of refactoring O(m^3) per round.
class DenseCholeskyInc {
public:
    explicit DenseCholeskyInc(std::size_t capacity) { m_rows.reserve(capacity); }

    // Append row m: `column[i]` = D(S[i], k) for the existing members S[0..m-1]
    // and `diagonal` = D(k, k). Returns false when positivity is lost.
    bool add(const std::vector<double>& column, double diagonal);

    // Solve (L L^T) x = b over the current m members.
    void solve(const std::vector<double>& b, std::vector<double>& x) const;

    [[nodiscard]] std::size_t size() const { return m_rows.size(); }

private:
    std::vector<std::vector<double>> m_rows;  // row i holds L(i, 0..i) incl. diagonal
};

}  // namespace cyber::remesh
