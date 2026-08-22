#include <doctest.h>

#include <cmath>
#include <cstddef>
#include <cstdlib>
#include <vector>

#include "../../src/quadrangulate/src/sparse_cholesky.hpp"
#include "support/scoped_env.hpp"

using cyber::remesh::SparseCholesky;

namespace {

// Full-pattern CSR of a k x k 5-point grid Laplacian with a +0.5 diagonal
// shift (SPD). Mesh-like sparsity: the case both orderings are built for.
struct Csr {
    std::size_t n = 0;
    std::vector<std::size_t> rowStart, colIndex;
    std::vector<double> value;
};

Csr gridLaplacian(std::size_t k) {
    Csr a;
    a.n = k * k;
    a.rowStart.push_back(0);
    for (std::size_t y = 0; y < k; ++y) {
        for (std::size_t x = 0; x < k; ++x) {
            const std::size_t i = y * k + x;
            std::vector<std::size_t> nbrs;
            if (x > 0) nbrs.push_back(i - 1);
            if (x + 1 < k) nbrs.push_back(i + 1);
            if (y > 0) nbrs.push_back(i - k);
            if (y + 1 < k) nbrs.push_back(i + k);
            a.colIndex.push_back(i);
            a.value.push_back(static_cast<double>(nbrs.size()) + 0.5);
            for (const std::size_t j : nbrs) {
                a.colIndex.push_back(j);
                a.value.push_back(-1.0);
            }
            a.rowStart.push_back(a.colIndex.size());
        }
    }
    return a;
}

std::vector<double> multiply(const Csr& a, const std::vector<double>& x) {
    std::vector<double> y(a.n, 0.0);
    for (std::size_t i = 0; i < a.n; ++i) {
        for (std::size_t p = a.rowStart[i]; p < a.rowStart[i + 1]; ++p) {
            y[i] += a.value[p] * x[a.colIndex[p]];
        }
    }
    return y;
}

// Factor under a forced ordering and check the solve against a manufactured
// solution; returns the factor size so callers can compare fill.
std::size_t factorAndCheck(const Csr& a, const char* ordering) {
    if (ordering != nullptr) {
        cyber::test::setEnv("CYBER_QC_ORDERING", ordering);
    } else {
        cyber::test::unsetEnv("CYBER_QC_ORDERING");
    }
    SparseCholesky chol;
    const bool ok = chol.factor(a.n, a.rowStart, a.colIndex, a.value);
    cyber::test::unsetEnv("CYBER_QC_ORDERING");
    REQUIRE(ok);

    std::vector<double> xTrue(a.n);
    for (std::size_t i = 0; i < a.n; ++i) {
        xTrue[i] = std::sin(static_cast<double>(i)) + 0.25;
    }
    const std::vector<double> b = multiply(a, xTrue);
    std::vector<double> x;
    chol.solve(b, x);
    double err = 0.0;
    for (std::size_t i = 0; i < a.n; ++i) {
        err = std::max(err, std::abs(x[i] - xTrue[i]));
    }
    CHECK(err < 1e-9);

    // solveUnitGather must agree with a full solve of e_k.
    const std::size_t k = a.n / 2;
    std::vector<double> ek(a.n, 0.0);
    ek[k] = 1.0;
    std::vector<double> full;
    chol.solve(ek, full);
    const std::vector<std::size_t> targets = {0, k, a.n - 1};
    double gathered[3];
    chol.solveUnitGather(k, targets, gathered);
    for (std::size_t t = 0; t < targets.size(); ++t) {
        CHECK(std::abs(gathered[t] - full[targets[t]]) < 1e-12);
    }
    return chol.factorNnz();
}

}  // namespace

TEST_CASE("sparse cholesky: AMD ordering solves exactly and fills less than RCM on a grid") {
    const Csr a = gridLaplacian(24);
    const std::size_t nnzRcm = factorAndCheck(a, "rcm");
    const std::size_t nnzAmd = factorAndCheck(a, "amd");
    const std::size_t nnzAuto = factorAndCheck(a, nullptr);
    // On a 2D grid AMD's nested-dissection-like orderings beat RCM's band.
    CHECK(nnzAmd < nnzRcm);
    // The default measures both symbolically and keeps the smaller factor.
    CHECK(nnzAuto == std::min(nnzRcm, nnzAmd));
}

TEST_CASE("sparse cholesky: AMD handles disconnected components and isolated vertices") {
    // Two disjoint grids plus one isolated vertex, assembled into one matrix.
    const Csr g = gridLaplacian(7);
    Csr a;
    a.n = 2 * g.n + 1;
    a.rowStart.push_back(0);
    for (std::size_t block = 0; block < 2; ++block) {
        const std::size_t off = block * g.n;
        for (std::size_t i = 0; i < g.n; ++i) {
            for (std::size_t p = g.rowStart[i]; p < g.rowStart[i + 1]; ++p) {
                a.colIndex.push_back(g.colIndex[p] + off);
                a.value.push_back(g.value[p]);
            }
            a.rowStart.push_back(a.colIndex.size());
        }
    }
    a.colIndex.push_back(a.n - 1);
    a.value.push_back(2.0);
    a.rowStart.push_back(a.colIndex.size());

    factorAndCheck(a, "amd");
}
