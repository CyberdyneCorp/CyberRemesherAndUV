# Tasks: add-injectable-layout

Deliberately front-loaded with measurement. This track has twice produced levers
that were correct in isolation and moved nothing observable, both times because
the gate measured something with no path to the output.

## Milestone 0 — attribute the failure

- [ ] M0. Split `badArcs` into named causes (empty row, lattice-free ordinal,
      fractional coefficient, excluded arc) and report them.
      Gate: the four counters sum to today's `badArcs` on every corpus model.

## Milestone 1 — make it a gate

- [ ] M1. Promote the counters onto `LayoutRunReport` so they are values rather
      than stderr, following the binding-parity path already established.
- [ ] M2. Script the injectability table so it is reproducible. Today's numbers
      are a docs claim from one unrecorded run.
      Gate: the script reproduces cube 1.00, bunny 0.26, rocker-arm 0.11,
      cheburashka 0.05, fandisk 0.05, spot 0.00 at 2000 quads.

## Milestone 2 — the joint blocker

- [ ] M3. Close `exclArcs`. Injection is gated on a conjunction that includes
      `exclArcs == 0`, so a perfectly injectable T-mesh injects nothing while
      arcs are still excluded. This is the parked Phase B work; state plainly
      that Phase B and injectability are a JOINT gate and neither moves output
      alone.

## Milestone 3 — architecture, only after 0-2

- [ ] M4. Choose between integer-grid-map cone pins, a half-integer lattice, and
      T-node quantization, with the measurement in hand. Record the options not
      taken and why, so they are not re-litigated.

## Milestone 4 — trust the gate

- [ ] M5. Mutation-verify before believing any green number: flip a constant
      that must matter and confirm the gate's numbers move AND the output hash
      changes. A gate that stays green under mutation is measuring nothing —
      the specific failure this whole change exists to correct.
