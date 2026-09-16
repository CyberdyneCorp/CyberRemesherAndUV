## Why

The C ABI facade combines callback marshalling, remesh request lowering, report
conversion, and exported entry points in one translation unit.  Its two remesh
orchestrators are difficult to exercise and review without changing ABI-facing
code.

## What Changes

- Move remesh-specific C-to-C++ lowering and C++-to-C report conversion into a
  private capability adapter.
- Give the adapter explicit request-lowering and report-conversion boundaries.
- Preserve every exported function, status, callback, allocation, and ABI
  layout.

## Capabilities

No behavior requirements change; this is an internal refactor.  The change
therefore opts out of spec deltas.

## Impact

`capi/src/capi.cpp`, a new private remesh adapter source module, CMake source
lists, and the C ABI regression tests.
