## Why

`CyberRemeshParams.adaptivity` is validated and recorded by the C ABI, but its
quad-cover and `CYBER_QUAD_ZREMESHER` factory paths replace the value with
uniform sizing. The CLI and dedicated ZRemesher entry point forward the same
value, so identical requests can produce different output according to the
entry point that a host uses.

## What Changes

- Forward the validated C ABI adaptivity value to both shared seamless-UV
  factory paths.
- Add parity regressions for the C ABI default quad-cover path and the
  `CYBER_QUAD_ZREMESHER` selector path.
- Record that default C ABI quad-cover output changes from uniform adaptivity
  to the canonical default of `1.0`, matching the CLI.

## Capabilities

### New Capabilities

- None.

### Modified Capabilities

- `remeshing-parameters`: Every supported entry point must forward adaptivity
  into its selected extractor rather than substituting a local default.

## Impact

- Affects `capi/src/capi.cpp`, C ABI consumers and all bindings built over it.
- No C ABI struct, function signature or ABI version changes.
- Default output for C ABI callers using quad-cover or its ZRemesher selector
  changes to match the already-shipped CLI default.
