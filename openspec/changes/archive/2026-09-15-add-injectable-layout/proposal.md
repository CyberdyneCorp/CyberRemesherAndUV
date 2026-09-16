# Make the topology layout reach the output on organic meshes

## Why

The ZRemesher track shipped an explicit `TopologyLayout` — nodes, arcs, patches,
validated and exportable — and on organic meshes **none of it reaches the output
mesh**.

The Bi-MDF assignment reaches the result only through INJECTION, and injection
needs an arc's symbolic length to reduce onto the solver's INTEGER free
variables. Measured at 2000 quads:

| model | arcs | non-injectable | injected |
|---|---|---|---|
| cube | 12 | **0** | works |
| stanford-bunny | 585 | 432 (74%) | 0 |
| rocker-arm | 736 | 653 (89%) | 0 |
| cheburashka | 648 | 616 (95%) | 0 |
| fandisk | 644 | 610 (95%) | 0 |
| spot | 438 | **438 (100%)** | 0 |

On a crease-pinned cube every arc runs between pinned crease isolines, so its
length IS an integer combination and the machinery works perfectly. On an
organic mesh a separatrix runs between T-nodes at CONTINUOUS positions, so its
length is not expressible in the integer basis and the arc is dropped.

This is why Phase B of `add-zremesher-retopology` measured every tracing lever
as byte-identical on output: recovering rejected orbits cannot matter while
74-100% of arcs are non-injectable. That phase's own re-assessment says the gate
"should be replaced by the injectability one before any further tracing work is
done". This change is that replacement.

It is scoped separately because it is a solver change of a different order from
the rest of the track — the joint half-integer lattice the roadmap already
records — and folding weeks of quantizer work into a change that is otherwise
complete would hold the finished parts hostage.

## What changes

Sequenced so the measurement exists before the architecture is chosen, because
this track has twice produced levers that looked right and moved nothing.

1. **Attribute the failure.** `badArcs` is one counter covering at least four
   distinct causes. Split it (empty row, lattice-free, fractional coefficient,
   excluded arc) and report the split.
2. **Make it a gate.** Promote the counters onto `LayoutRunReport` so they are
   readable as values — the binding-parity work already established that path —
   and script the table above so it is reproducible rather than a docs claim
   from one unrecorded run.
3. **Close `exclArcs` first.** Injection is gated on a conjunction that includes
   `exclArcs == 0`, so a perfectly injectable T-mesh still injects nothing while
   arcs remain excluded. Phase B and injectability are a JOINT gate; neither
   moves output alone, which is precisely why Phase B read as inert.
4. **Then choose the architecture** — integer-grid-map cone pins, a half-integer
   lattice, or T-node quantization — with the measurement in hand.

## Impact

- Affected specs: `remeshing-pipeline` (a new injectability requirement)
- Affected code: `src/quadrangulate/src/seamless_solver.cpp` (the Bi-MDF
  reduce/inject path), `LayoutRunReport`
- No behaviour change until step 4; steps 1-3 are instrumentation and gating

**Success is two numbers, not one.** `injectableFraction` alone can be driven to
1.0 without changing a single output mesh — the failure mode this track has hit
repeatedly. It must be paired with a measure of output reach (realized arc
deviation against the Bi-MDF optimum), and the gate must be mutation-verified
before it is trusted.
