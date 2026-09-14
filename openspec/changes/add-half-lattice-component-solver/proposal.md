## Why

Organic layouts contain arcs ending at continuous T-nodes, so their lengths do
not reduce to the existing integer-free variables. The current exact-injection
gate consequently injects no organic layout, while partial pinning has been
measured to worsen realized layout-deviation energy.

## What Changes

- Add an opt-in half-lattice component solver between the Bi-MDF target solve
  and seamless-parameterization rounding.
- Treat the relation between layout arcs and reduced variables as disconnected
  components, and solve or decline each component as a whole.
- Lift eligible components onto a doubled lattice and enforce their parity
  constraints exactly; do not mix an injected component with greedy values.
- Report component eligibility, rejection reasons, parity consistency, and
  output-reaching injection separately from existing per-arc diagnostics.
- Define corpus and mutation gates required before the new solver becomes the
  default output path.

## Capabilities

### New Capabilities

- `half-lattice-layout-injection`: Safely realizes compatible Bi-MDF layout
  assignments through complete doubled-lattice components.

### Modified Capabilities

- `remeshing-pipeline`: The seamless quantizer can select the component solver
  and reports whether an eligible component reached final integer rounding.

## Impact

Affected code is centered on the Bi-MDF row reduction and injection path in
`src/quadrangulate/src/seamless_solver.cpp`, with new internal component and
diagnostic types. It reuses the existing topology-layout and Bi-MDF assignment;
it does not change the C ABI in the first implementation milestone.
