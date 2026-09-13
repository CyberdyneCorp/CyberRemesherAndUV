## Context

The current benchmark has generated fixtures, optional downloads, solver-aware
baselines, and a hosted CI lane. It does not yet make the corpus itself a
versioned contract, distinguish invalid geometry from quality drift, or retain
enough run identity to reproduce a result without reading CI logs.

## Goals / Non-Goals

**Goals:**

- Keep the deterministic gate offline, small, and runnable in ordinary CI.
- Treat invalid output, missing required fixture data, and a failed subprocess
  as failures rather than absent measurements.
- Produce self-contained JSON records suitable for a review artifact.

**Non-Goals:**

- Establishing ZRemesher parity or cross-toolchain bit identity.
- Bundling large third-party scans in the source tree.
- Turning hosted CI wall-clock time or RSS into a portability gate; those need
  controlled device and release-hardware measurements.

## Decisions

### A checked-in manifest separates acceptance fixtures from optional downloads

The acceptance tier is procedural and deterministic, with a manifest that
records its schema version, fixture class, parameters, expected input outcome,
and generated-content hash. Optional downloaded comparisons remain explicitly
non-gating and retain URL, licence, and content hash.

This avoids network availability becoming a merge prerequisite while ensuring
that a fixture edit cannot silently change what the gate measures. Storing all
production scans in Git was rejected because their size and redistribution
terms make the core test suite impractical.

### Validity is a first-class metric group

The harness will calculate structural validity before quality metrics: face
index/range and degeneracy checks, edge/vertex manifoldness, boundary-loop
counts, and broad-phase self-intersection candidates followed by triangle
checks. Invalid outputs receive an explicit validity failure and cannot be
accepted by a favorable aesthetic metric.

An external mesh-library dependency was rejected: the metrics must run in the
same offline Python environment already used by the benchmark lane.

### Records pin the comparison context

Each result record contains corpus identity, input hash, command, requested and
achieved counts, seed, solver identity, ABI identity where available, build
identity, platform/toolchain, metric version, elapsed time, and an output or
failure-artifact path. Baselines store the same identity fields relevant to
comparison and reject incompatible records.

This is stronger than embedding a free-form comment in a baseline and avoids
mistaking a changed solver build or fixture for a quality regression.

### Correctness and performance gates remain distinct

Hosted CI enforces corpus completeness, deterministic quality and topology
validity. A documented release command emits performance/RSS fields but does
not assign a universal time threshold; controlled device profiles can consume
the same JSON schema later.

## Risks / Trade-offs

- [Triangle-pair intersection is expensive] → Use it only on the compact
  acceptance tier, apply a bounding-box broad phase, and make the sampling and
  exactness of each metric explicit in output.
- [Procedural fixtures underrepresent assets] → Keep the optional licensed
  comparison tier and require fixture categories that exercise known failure
  modes; adding a licensed fixture is a manifest review, not an ad-hoc URL.
- [Different toolchains differ discretely] → Baselines remain toolchain and
  solver identity-specific; the check rejects incomparable identities.

## Migration Plan

Add the manifest and metric version alongside the existing generated corpus,
record fresh approved baselines, then switch the CI lane to require the
acceptance tier. The old baseline files remain readable only while they carry
the fields needed for a compatible record; reverting the CI invocation restores
the existing generated-only check.
