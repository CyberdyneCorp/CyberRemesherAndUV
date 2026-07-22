#include <doctest.h>

#include "cyber/quadrangulate/mcf_layout.hpp"

// Min-cost-flow integer layout, M1 — proves the vendored SciPP max-flow is linked
// and functional in-tree. Only built when CYBER_WITH_SCIPP is enabled (the
// dependency-free default build does not link SciPP). See
// docs/mcf-integer-layout-plan.md.
TEST_CASE("SciPP maximum_flow is linked and computes the known max-flow") {
    REQUIRE(cyber::remesh::mcfMaxFlowSelfTest());
}
