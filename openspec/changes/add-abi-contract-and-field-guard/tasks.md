# Tasks: add-abi-contract-and-field-guard

## ABI contract

- [x] A1. `CYBER_ABI_VERSION_MAJOR` / `_MINOR` in the header, with the policy
      stated where a consumer reads it, and CMake READING the header rather than
      defining the value.
- [x] A2. `cyber_abi_version` and `cyber_abi_check`, reusing the existing
      `CYBER_ERR_INCOMPATIBLE_VERSION`. `cyber_version` untouched.
- [x] A3. `SOVERSION` = the ABI major, and the Python loader's `_SOVERSION`
      moved with it — they are two declarations of one number in different
      languages, and nothing else compared them.
- [x] A4. Python (`abi_version`, `check_abi`) and Swift (`abiVersionComponents`,
      `checkABI`) surfaces; the Swift file's "the C ABI carries NO separate ABI
      version symbol" comment deleted, because it is no longer true.
- [x] A5. Tests, including the spec's own previously-unwritable scenario, and
      `static_assert`s pinning the sizeof of the array-strided structs.
- [x] A6. A checked-in, type-aware layout manifest generated from the public
      header and compiler-measured on every CTest toolchain. It records function
      and callback signatures, enum values/representation, every struct field's
      declared type/name/offset/size and each aggregate's size/alignment, so it
      detects both `float* -> double*` and a field added in existing trailing
      padding. The test mutates both cases. A v0.8.0 header subset also compiles
      as its own translation unit, links to the current library and checks
      guarded out-param/array writes.

## Field-evaluator boundary

- [x] B1. `GuardedField` in `bake.cpp` routing every callback, with the two-tier
      policy. NaN distance is Tier 1 and infinity is Tier 2 — deliberately, and
      not arbitrarily: `std::fmax(NaN, epsilon)` returns `epsilon`, so a NaN
      never stopped the march at all.
- [x] B2. The fifth callback site: `curvature()`'s interface default probes
      `gradient()` six times OFF the surface, outside any wrapper. It now
      propagates a non-finite probe instead of letting `normalized()` launder it
      to `{0,0,0}` and returning a finite curvature computed from nothing.
- [x] B3. `curvatureScale` filters non-finite samples. `NaN != 0.0f` is true, so
      a NaN reached `weightedPercentile`'s `std::sort`, where comparing it
      violates strict weak ordering — UB in the sort, not just a poisoned range.
- [x] B4. The C ABI gradient out-param seeded with NaN instead of `{0,0,1}`.
- [x] B5. Python trampolines record the exception, return NaN, and `bake_field`
      re-raises it — instead of substituting `0.0`, which satisfies
      `|d| <= epsilon` and reported a hit at the cage origin.
- [x] B6. Hostile-field tests, each verified to FAIL with its guard reverted.
- [ ] B7. Lipschitz-violation detection (a sign flip between consecutive
      samples). DEFERRED: speculative, and the header already states the bound
      cannot be enforced.
