## 1. Job lifecycle

- [x] 1.1 Implement a lock-protected single-execution remesh job and shared terminal result.
- [x] 1.2 Add explicit idempotent cancellation and bridge task cancellation to it.
- [x] 1.3 Ensure progress and callback state finish exactly once after the native worker returns.

## 2. iPadOS integration and documentation

- [x] 2.1 Make the iPadOS cancel action call the running operation directly.
- [x] 2.2 Document ownership, repeat-await and cancellation semantics in the Swift README.

## 3. Verification

- [x] 3.1 Add lifecycle regression tests using an injectable job runner.
- [x] 3.2 Run Swift package tests, CTest binding parity, and strict OpenSpec validation.
