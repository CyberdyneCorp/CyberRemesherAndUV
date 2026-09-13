## Why

Hosts need to retopologise a selected area without damaging finished exterior
topology. The current library offers global remeshing and manual boundary
tools, but neither defines a transactional selected-region operation, frozen
border semantics, or a usable mapping back to the source surface.

## What Changes

- Add a partial-retopology operation selected by source face IDs.
- Support exact frozen borders: keep boundary vertex identity, position and
  order, and leave exterior faces and connectivity unchanged.
- Reject infeasible selections before mutation, including non-manifold or
  non-simple boundaries and boundary parity that cannot satisfy the requested
  all-quad policy.
- Return source-face/barycentric surface correspondence and explicit transfer
  status for preserved, interpolated and unsupported attributes.
- Expose the operation and report through the additive C ABI, Python and Swift
  bindings; a cancelled or failed operation leaves the input unchanged.

## Capabilities

### New Capabilities

- `partial-retopology`: selected-region remeshing, frozen-border validation,
  atomic stitch semantics and source correspondence.

### Modified Capabilities

- `engine-bindings`: C, Python and Swift access to partial-retopology reports.
- `application-shell`: document transactions for a partial-retopology result.

## Impact

Core mesh and pipeline region extraction, reference-surface correspondence,
C ABI minor version, Python and Swift bindings, document mutation/undo
integration, and core/C/Python/Swift regression tests.
