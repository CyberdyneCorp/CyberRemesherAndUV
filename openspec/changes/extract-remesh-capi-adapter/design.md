## Context

The public C ABI must remain a thin exception-contained boundary.  Remesh
entry points currently own conversion of caller POD, resource caps, guidance,
and result reports in addition to pipeline control flow.

## Goals / Non-Goals

**Goals:** isolate deterministic data translation behind private types, make
ownership explicit, and retain the existing C entry-point behavior.

**Non-Goals:** change remeshing algorithms, statuses, callback timing, public
headers, or ABI version.

## Decisions

- Add a private `remesh_adapter` module with a `LoweredRequest` value type.
  It owns only lowered parameter and optional resource-limit state, never mesh
  ownership or caller callback pointers.
- Keep the exported entry points and their exception containment in
  `capi.cpp`; they delegate data translation to the adapter.
- Keep report output caller-owned.  Conversion overwrites only ABI POD output
  structures and retains the existing semantic-boundary capacity contract.

## Risks / Trade-offs

Splitting translation code can accidentally alter handling of null buffers or
oversized counts.  Existing ABI tests cover these paths; new adapter-focused
tests cover request lowering and report conversion without invoking a solver.

## Validation

Run the C API test target, the full configured CTest suite, formatting checks,
and a cognitive-complexity measurement for the touched facade.
