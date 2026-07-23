# Proposal: align-quality-claims-with-measurement

## Why

`remeshing-pipeline` carries a normative quality guarantee the benchmark does not
support:

> on the organic test corpus it SHALL match or beat QuadriFlow on median quad
> angle AND irregular-vertex count

That sentence entered the spec from a "3/4 organic models" reading that was
selection bias — it counted **rocker-arm**, a mechanical part, as organic and
omitted **cheburashka**, an actual organic character. Cheburashka loses *both*
named axes (median 80 vs 82, irregular 4% vs 2%) and carries the corpus's widest
edge-length-CV gap (0.22 vs 0.15). On the real organic set the default is **2/3**,
not 3/4; corpus-wide it is **3/5 on median and 3/5 on irregular**.

The claim also became measurable only recently: the benchmark had been scoring the
retired `instant-meshes` extractor in its comparison phases, and was re-pointed at
the shipped default in `7abbaec`. The numbers below are from that corrected
harness.

Meanwhile the spec **understates** the result that is actually corpus-wide and
unambiguous: the default is topologically valid on **5/5**, including cases where
QuadriFlow tears the mesh (cube: 0 defects vs 554; stanford-bunny: 8 vs 38). The
strongest true claim is not in the spec, and a weaker false one is.

This change replaces the aspiration with the measurement, and records the two
known losses so they are tracked rather than implied away.

## What Changes

- **Narrow the quality guarantee** in the default-extractor scenario from "the
  organic test corpus … median AND irregular" to the measured majority (spot,
  rocker-arm, stanford-bunny), naming **fandisk and cheburashka** as the known
  exceptions.
- **Promote topological validity to a normative guarantee** — it holds on 5/5 and
  is the one axis where the default beats QuadriFlow outright.
- **State the feature-following gap as a known limitation** rather than leaving it
  unmentioned: 3 real losses (fandisk 2.0x, cheburashka 2.0x, rocker-arm 1.5x),
  with spot a tie and stanford-bunny confounded by its scan-hole boundary.

No code changes. This is a spec-correctness change: the engine is unchanged and
the benchmark already reports these numbers.

## Capabilities

- `remeshing-pipeline` — MODIFIED: quality claims restated against measurement.

## Impact

- Anyone reading the spec as a contract gets the guarantee the code actually
  meets. The previous text would fail a conformance check on cheburashka.
- The feature-following gap becomes a documented, tracked limitation instead of an
  unstated one; see `docs/ROADMAP.md` for the three cheap levers already measured
  dead (M2a vertex-snap, M2b gauge-pin, M2c routing-threshold).
