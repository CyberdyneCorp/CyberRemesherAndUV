# Tasks: add-injectable-layout

Deliberately front-loaded with measurement. This track has twice produced levers
that were correct in isolation and moved nothing observable, both times because
the gate measured something with no path to the output.

## Milestone 0 — attribute the failure

- [x] M0. Split `badArcs` into named causes (empty row, lattice-free ordinal,
      fractional coefficient, excluded arc) and report them.
      Gate: the four counters sum to today's `badArcs` on every corpus model.

## Milestone 1 — make it a gate

- [x] M1. Promote the counters onto `LayoutRunReport` so they are values rather
      than stderr, following the binding-parity path already established.
- [x] M2. Script the injectability table so it is reproducible. The script
      validates exclusive causes on every row; stale historic ratios remain
      documented measurements, not a baseline for the changed solver.

## Milestone 2 — the joint blocker

- [x] M3. Establish that `exclArcs` and non-liftable organic rows are a joint
      exact-injection gate. Exact partial pinning remains unsafe; the selected
      integral-rounding path does not require falsely claiming that either
      blocker alone is solved.

## Milestone 3 — architecture, only after 0-2

- [x] M4. Choose guided integral rounding as the evidence-backed alternative
      to integer-grid-map pins and a joint half-integer lattice; record the
      rejected approaches, feasibility limits, and fallback in `design.md`.

## Milestone 4 — trust the gate

- [x] M5. Mutation-verify the forced Bi-MDF pinning path: the offline CTest
      requires changed injected-pivot state and a changed output hash.
