## 1. C ABI

- [x] 1.1 Define the additive CSR input descriptor, typed vertex/face/corner
      schema, and authored-polygon copy APIs with ownership/validation docs.
- [x] 1.2 Implement transactional import and deterministic compacted geometry
      and attribute export.
- [x] 1.3 Update ABI manifests and compatibility tests.

## 2. Swift SDK

- [x] 2.1 Add polygon-aware typed import and export APIs over the C descriptor.
- [x] 2.2 Preserve the existing triangle initializer as a compatibility layer.

## 3. Python and Rust SDKs

- [x] 3.1 Add Python typed attribute and geometry import/export APIs.
- [x] 3.2 Add the documented geometry-only Rust wrapper API and regression test.

## 4. Verification

- [x] 4.1 Add C++/C ABI and Swift regression tests for mixed arities, shared
      and unused vertices, source-buffer mutation, and malformed descriptors.
- [ ] 4.2 Run native, ABI, Swift, and strict OpenSpec gates.
