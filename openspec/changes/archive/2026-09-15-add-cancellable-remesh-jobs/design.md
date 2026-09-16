## Context

The C ABI is synchronous and invokes progress and cancellation callbacks through
one retained opaque user pointer. The Swift wrapper presently starts a detached
native call for every `value()` invocation. Its iPadOS consumer cancels a
separate task that reads progress, so it does not request cancellation from the
native job. See proposal.md for the motivation and the modified contracts.

## Goals / Non-Goals

**Goals:**

- Make a Swift `RemeshOperation` a thread-safe, single-execution job with a
  shared terminal result.
- Preserve the C callback pointer for the complete native call and serialize
  progress delivery through the existing stream.
- Make explicit and task cancellation reach the same cancellation flag.
- Exercise the lifecycle without requiring an engine solve in unit tests.

**Non-Goals:**

- Add a new C ABI job-handle API or change process-wide backend/thread-cap
  controls in this change.
- Promise a deadline beyond the native pipeline's existing cooperative polling
  contract.

## Decisions

- Use a lock-protected Swift control state for created/running/cancelling and
  terminal states. It coordinates callback state, the single detached worker,
  and all continuations. This is preferable to actor-only state because C
  callbacks are synchronous and cannot await an actor.
- `value()` joins the existing job instead of rejecting a second await. Joining
  is natural for view, progress and application tasks and removes a race that
  could otherwise run the same mesh twice.
- `cancel()` only latches a cooperative request. It never resumes awaiters or
  releases callback storage early; the worker remains responsible for publishing
  one terminal result after the C call returns.
- The iPad app explicitly calls `op.cancel()`. Cancelling the progress pump only
  stops UI observation and is not a cancellation mechanism.

## Risks / Trade-offs

- [A native backend cannot poll while inside an opaque long call] → the wrapper
  retains all state until it returns and does not claim a stronger deadline.
- [Detached worker and Swift strict-concurrency diagnostics] → only immutable
  job inputs and the lock-protected control object cross the worker boundary.
- [A caller never observes progress] → newest-value buffering bounds memory and
  does not affect execution or cancellation.

## Migration Plan

The public operation gains `cancel()` without an ABI change. Existing callers
that await once continue to work; callers that accidentally await twice now join
one execution. Rollback is a source-compatible reversion of the Swift wrapper.
