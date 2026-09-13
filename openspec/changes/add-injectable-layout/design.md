## Context

An inspected `TopologyLayout` and an optimal Bi-MDF arc assignment do not by
themselves alter an extracted mesh.  The original exact-injection path can pin
only values that lift through the seamless solver's integer free-variable
basis.  Organic T-nodes introduce continuous coordinates, so their arc rows
are commonly lattice-free or fractional.

## Measurements and decision

The current diagnostic script records mutually exclusive arc causes and checks
that they reconcile with the total.  The measured corpus evidence is retained
in `docs/ROADMAP.md` and `docs/zremesher-plan.md`: exact injection reaches a
crease-pinned cube, while organic layouts retain a large lattice-free/fractional
remainder.  Contained regions add a separate excluded-arc constraint; removing
only one constraint cannot make partial exact pinning safe.

Three approaches were evaluated:

1. **Integer-grid-map cone pins** preserve the existing lifting path but do
   not represent T-node positions, so they leave the organic remainder.
2. **Joint half-integer lattice quantization** could represent the target
   algebraically, but the required parity coupling across seams and cones is
   not implemented or validated; forcing partial rows was measured to regress
   neighbouring strips.
3. **Guided integral rounding** is selected.  It keeps the existing greedy
   integer schedule and seamless invariants, while adding a Bi-MDF residual
   attraction before rounding.  It therefore realizes layout information in
   the final integer map without asserting that an unrepresentable partial
   assignment is exact.

## Safety and fallback

The exact path remains opt-in and requires complete, clean coverage.  Guided
rounding is deterministic and falls back to the unchanged greedy schedule when
the layout solve is unavailable, infeasible, cancelled, or produces no usable
steering rows.  The C API reports injectability statistics so hosts can
distinguish a fallback from a successful exact injection.

## Validation

`tools/bench/injectability.py` is now a CTest regression.  It runs the
offline generated corpus, reconciles every arc-cause partition, and forces a
feasible pinning perturbation that must change the output mesh hash.  Organic
corpus measurements remain an explicit release artifact rather than a
machine-specific CI baseline.
