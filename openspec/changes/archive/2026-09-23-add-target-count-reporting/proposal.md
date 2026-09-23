## Why

`targetQuadCount` is a request, not an exact-count guarantee, but callers
cannot currently tell what target the pipeline actually calibrated against,
which attempt it retained, or why the achieved count differs. Hosts therefore
repeat expensive remeshes blindly and can mistake pure-quad expansion or an
infeasible constrained island for a solver failure.

## What Changes

- Record requested, effective base, calibrated, and final output counts for a
  run and every island, including the retained calibration attempt and explicit
  termination reason.
- Add an opt-in bounded count policy with tolerance and maximum attempts;
  preserve current two-attempt/default behavior when it is not requested.
- Make count candidates eligible only after geometry/border validity checks;
  report tolerance misses and exhausted budgets separately from cancellation or
  pipeline errors.
- Publish the result through the C ABI, Python and Swift bindings and the CLI
  JSON report, without changing existing caller-owned parameter structs.

## Capabilities

### New Capabilities

- `target-count-policy`: bounded, observable target-count calibration and its
  termination semantics.

### Modified Capabilities

- `remeshing-parameters`: add the optional policy without changing defaults.
- `remeshing-pipeline`: report count allocation and achieved results per island.
- `engine-bindings`: expose the additive policy/report ABI to language hosts.
- `cli-headless`: include count outcomes in the machine-readable report.

## Impact

The pipeline and QuadCover extractor gain result metadata and optional bounded
search controls. The C ABI gains additive sibling structs/functions; Python,
Swift and CLI reports gain corresponding representations. No current parameter
layout or default output behavior changes.
