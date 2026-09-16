## Why

The Swift remesh wrapper exposes an operation but does not yet guarantee one
native execution, an explicit cancellation path, or ordered callback delivery.
That makes the API unsafe for an interactive iOS host and prevents the existing
cooperative-cancellation guarantee from being relied upon at the job boundary.

## What Changes

- Define `RemeshOperation` as a single-execution job with an explicit,
  idempotent cancellation operation and a shared terminal result for all
  awaiters.
- Make job ownership, callback delivery, input lifetime, result lifetime and
  cancellation semantics explicit in the supported Swift API and documentation.
- Wire the iPadOS shell's Cancel control to the remesh job, preserving its
  existing atomic document commit/rollback behaviour.
- Add regression coverage for repeated awaits, explicit cancellation and the
  shell cancellation path.

## Capabilities

### New Capabilities

None.

### Modified Capabilities

- `engine-bindings`: Define the lifecycle, ownership and cancellation contract
  of the Swift async remesh job surface.
- `application-shell`: Require the iPadOS long-operation UI to cancel the
  underlying remesh job rather than only its progress observer.

## Impact

Affected code is the Swift remesh wrapper, its package tests and README, the
iPadOS app model, and their OpenSpec contracts. The C ABI remains compatible;
this change makes the existing callback-based C remesh call safe to use through
the supported Swift concurrency surface.
