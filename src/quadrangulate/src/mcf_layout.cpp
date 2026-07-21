#include "cyber/quadrangulate/mcf_layout.hpp"

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

}  // namespace cyber::remesh
