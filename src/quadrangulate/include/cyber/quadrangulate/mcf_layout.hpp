#pragma once

// Min-cost-flow integer layout (docs/mcf-integer-layout-plan.md) — the port of
// QuadriFlow's integer-layout stage onto the vendored, dependency-free SciPP
// max-flow. This header will grow the layout API across M2–M5; for M1 it exposes
// only a self-test that proves the SciPP toolchain is linked and functional.
namespace cyber::remesh {

// Runs SciPP's csgraph::maximum_flow on a tiny known network (max-flow == 5) and
// returns true iff it computes the expected value. Returns false when the module
// was built without CYBER_WITH_SCIPP. Exercised by the gated unit test.
[[nodiscard]] bool mcfMaxFlowSelfTest();

}  // namespace cyber::remesh
