## Context

See [proposal.md](proposal.md) for motivation. The current injection path
reduces each arc expression into integer-free variables, discards rows touching
continuous frees, then uses floating Gauss-Jordan elimination. It can only
inject when every pivot is close to integral and no arc was excluded by the
tracer, Bi-MDF infeasibility valve, or local elimination. This is safe but
reaches none of the measured organic layouts.

The measured corpus distinguishes two coupled issues: rejected topology orbits
produce excluded arcs, while most organic arcs are lattice-free because their
endpoints are continuous T-nodes. Increasing accepted polygon size reduced
some exclusions but increased lattice-free arcs; forced partial pinning changed
the bunny result while worsening its realized deviation energy. The solver must
therefore model the whole coupled relation, not waive individual gates.

## Goals / Non-Goals

**Goals:**

- Build a deterministic bipartite component graph from arc equations and all
  reduced degrees of freedom they depend on.
- Represent eligible relations in doubled-lattice coordinates and solve the
  induced GF(2) parity system exactly before choosing integer assignments.
- Make component admission atomic: an injected component has neither unknown
  external dependencies nor fallback-owned variables.
- Establish observability and staged quality gates before changing defaults.

**Non-Goals:**

- Accepting invalid/rejected topology-layout orbits just to improve coverage.
- Post-hoc quantization or relocation of T-nodes after layout tracing.
- Replacing the Bi-MDF optimizer, changing its patch templates, or promising
  ZRemesher-quality output from this design alone.
- Exposing internal component equations in the public ABI during the initial
  rollout.

## Decisions

### Model the complete relation as components

Create a bipartite graph with equation nodes for layout-arc constraints and
variable nodes for every reduced free referenced by the arc expression. Add
sentinel dependency nodes for continuous frees, excluded arcs, fractional
coefficients, and fixed/external quantities. Enumerate vertices and adjacency
in existing stable arc/free order, then traverse deterministically.

An injection candidate is a connected component, not an equation nor a pivot.
Only a closed component can be admitted: all of its equations are supported,
all variables are owned by it, and it has no sentinel dependency. This directly
prevents the known forced-mode failure in which a clean pivot is pinned while a
connected greedy variable realizes a different value.

We reject the current global floating elimination as the component decision
mechanism: its pivot order makes local cleanliness look independent when the
parity class is global. We also reject a component graph only over currently
integer-free variables: it would hide precisely the continuous T-node relations
that make an organic component unsafe.

### Use doubled coordinates and a GF(2) parity pre-solve

Normalize each closed component equation into doubled coordinates. If an arc
constraint is `sum(c_i x_i) = h/2`, use integer doubled coordinates `z_i =
2x_i` and clear denominators to form an integer system. Reduce coefficient and
right-hand-side parity modulo two, solve connected parity constraints by
deterministic Gaussian elimination over GF(2), and retain a witness row for any
contradiction.

Parity feasibility is necessary, not sufficient. After it succeeds, solve the
integer affine system subject to the parity class and existing `tCap` bounds,
then substitute its values into every original equation. An assignment is
eligible only if every target residual is within the existing exact tolerance
and every final value maps to a legal integer assignment for the seamless
quantizer. No unresolved half-step is rounded independently.

This uses the actual half-lattice structure exposed by the current diagnostic,
rather than cone pins (which cannot represent continuous T-node endpoints) or
T-node quantization (which mutates tracing after patch constraints were solved).

### Separate safety classification from policy

The component builder and solver always run in report-only mode first. Their
classification is independent of environment policy. A runtime selector then
chooses `off`, `report`, or `inject`; `inject` uses only accepted components.
The legacy `CYBER_QC_BIMDF` guided/greedy path remains the island fallback.

The first implementation is internal and opt-in. Existing per-arc
`LayoutRunReport` metrics remain intact; additive component counters are
threaded through the established C++/CLI/Python report path only after their
semantics are fixed by tests. If those counters require a public C report
extension, that is a separate ABI-minor task with its own manifest change.

### Gate promotion by complete evidence, not coverage alone

Component injection is promotable only after reproducible corpus runs show:

1. deterministic component census and output hashes;
2. no invalid layout, invalid mesh, non-finite output, or pure-quad regression
   on the applicable gate corpus;
3. a nonzero number of complete components and injected arcs on at least one
   organic model, with an output hash different from report/fallback mode;
4. realized arc-deviation energy no worse than fallback for each model where
   components were injected, plus recorded angle, edge-CV, and irregularity
   comparisons; and
5. mutation evidence that corrupting parity or component ownership changes both
   component diagnostics and the injection-enabled output.

The default remains unchanged until these gates are accepted. A component
solver that cleanly declines all organics is diagnostic progress, not M3
completion.

## Risks / Trade-offs

- [The real relation includes non-linear or non-half coefficients] → classify
  those dependencies explicitly; decline their complete components rather than
  approximating them.
- [Integer affine solve becomes expensive for a large component] → use sparse
  component-local storage, cap diagnostic work, and fall back on a deterministic
  resource-limit rejection with a reported reason.
- [Component boundaries are incorrectly inferred] → unit-test synthetic
  disconnected, shared-variable, sentinel, and cycle cases; substitute every
  accepted result back into original rows.
- [A mathematically valid assignment harms mesh quality] → preserve the
  island-level layout/mesh/quality rollback gate and keep the feature opt-in.
- [Diagnostics change across platforms] → avoid hash traversal and floating
  pivot choices in classification; use stable indices and exact integer/GF(2)
  operations for admission.

## Migration Plan

1. Add pure internal data types and tests for normalization, component
   discovery, GF(2) feasibility, witnesses, and atomic ownership.
2. Integrate report-only classification beside the existing injection census;
   verify legacy output is byte-identical.
3. Add the bounded affine solver and per-component substitution checks, still
   report-only; establish corpus component baselines.
4. Add opt-in atomic injection with island rollback to legacy guided/greedy on
   any post-solve gate failure.
5. Run the full corpus and mutation checks. Promote only after the documented
   gates pass; otherwise retain the opt-in implementation and record the
   observed blocker.

## First admission sweep

The opt-in endpoint was verified on the crease-pinned box at 200 target quads:
three complete components covering 12 arcs changed the output hash only with
`CYBER_ZR_HALF_LATTICE=inject`; default output stayed unchanged. This proves
the endpoint is wired through extraction, but is not the organic gate.

On the generated sphere (six tessellations, targets 30–400) and torus (targets
20–2000), no component both avoided excluded ownership and passed exact
residual validation. The one low-resolution sphere component that was isolated
after exclusions failed exact residual validation. Organic progress therefore
requires target-consistent component construction or a projection of Bi-MDF
targets onto a legal component assignment; partial pinning is not permitted.
