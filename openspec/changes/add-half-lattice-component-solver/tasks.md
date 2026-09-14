## 1. Component model and exact arithmetic

- [ ] 1.1 Define internal equation, dependency-sentinel, component, rejection,
      and parity-witness types without changing quantizer behavior.
- [ ] 1.2 Normalize supported arc equations into checked doubled-lattice
      integer rows; classify overflow and non-half coefficients explicitly.
- [ ] 1.3 Build stable bipartite components over equations, reduced variables,
      and sentinel dependencies.
- [ ] 1.4 Implement deterministic GF(2) parity solving with contradiction
      witnesses and unit tests for satisfiable and unsatisfiable cycles.

## 2. Complete component admission

- [ ] 2.1 Implement the bounded integer-affine completion after parity solving.
- [ ] 2.2 Substitute every candidate result into its original rows and enforce
      target residual, integer-domain, and ownership checks.
- [ ] 2.3 Reject entire components for unsupported relation, parity conflict,
      bound violation, residual, or resource limit; add regression tests that
      prove no variable from a rejected component is pinned.

## 3. Observability and report-only integration

- [ ] 3.1 Add deterministic component counters and named rejection reasons to
      the internal run report while preserving existing per-arc metrics.
- [ ] 3.2 Add a report-only runtime mode and tests proving byte-identical legacy
      output with stable component diagnostics.
- [ ] 3.3 Extend the layout-injectability corpus harness to record component
      census, parity results, and fallback output hashes.

## 4. Guarded injection and rollback

- [ ] 4.1 Add an opt-in selector that applies only complete admitted components
      in one batch and leaves all other components to guided/greedy rounding.
- [ ] 4.2 Add island-level rollback when layout validity, mesh validity,
      non-finiteness, or component residual checks fail; report the gate that
      triggered fallback.
- [ ] 4.3 Add tests for mixed eligible/rejected components and for no partial
      pinning across a component boundary.

## 5. Corpus gates and promotion decision

- [ ] 5.1 Establish and commit deterministic report-only component baselines
      for cube, spot, fandisk, rocker-arm, cheburashka, and stanford-bunny.
- [ ] 5.2 Run opt-in corpus comparisons for output hashes, layout/mesh validity,
      realized deviation energy, median angle, edge-length CV, and irregularity.
- [ ] 5.3 Mutation-verify parity and ownership gates, confirming diagnostics and
      injection-enabled output both change.
- [ ] 5.4 Record the promotion decision; make the solver default only if every
      design gate passes, otherwise retain opt-in and document the remaining
      blocker.
