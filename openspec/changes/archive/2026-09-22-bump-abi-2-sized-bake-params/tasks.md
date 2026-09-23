## 1. C ABI

- [x] 1.1 `CYBER_ABI_VERSION_MAJOR 2`, `CYBER_ABI_VERSION_MINOR 0`; confirm the
      soname follows (`libcyber_capi.so.2` / `.2.dylib`).
- [x] 1.2 `size_t structSize` as the FIRST member of `CyberBakeParams` and
      `CyberBundleParams`; drop the stale "Appended in 0.8.0 / 0.9.0" annotations.
- [x] 1.3 One sized-struct rule in the header covering the provider descriptors and
      these two, replacing the "one documented exception" wording.
- [x] 1.4 `cyber_default_bake_params` / `cyber_default_bundle_params` return
      `CyberStatus`; refuse `NULL` and a size below the floor, naming both numbers.
- [x] 1.5 One size-aware reader in `capi.cpp`, shared with the provider descriptors,
      overlaying the caller's bytes on engine defaults; floors frozen by `offsetof`.
- [x] 1.6 `applyBakeParams` (cyber_bake, cyber_bake_field, cyber_bake_udim, the
      provider's request) and `cyber_export_bundle_write` read through it.

## 2. Bindings

- [x] 2.1 Python: `structSize` in both ctypes structures, set before the default
      call; default calls return a status that is checked; `_SOVERSION = 2`;
      `ABI_VERSION_MAJOR / MINOR` follow the header.
- [x] 2.2 Swift: `structSize` set in both C-struct builders; default calls checked.
- [x] 2.3 Rust: nothing binds these structs; soname prose that names `.so.1` updated.

## 3. Tests

- [x] 3.1 `cyber_abi_check`: a 1.x client is refused, 2.0 is served.
- [x] 3.2 A default call with `structSize` unset/below the floor is refused and
      leaves the struct untouched.
- [x] 3.3 A default call writes nothing past `structSize` (canary past the struct).
- [x] 3.4 A caller stating a size LARGER than this build's layout is served, and its
      trailing bytes are neither read as parameters nor written.
- [x] 3.5 Every existing C, Python and Swift caller sets `structSize`; the full suite
      and both binding-parity gates stay green.
- [x] 3.6 `capi/abi/cyber_capi-2.0.json` generated and pinned.

## 4. Docs

- [x] 4.1 CHANGELOG: a breaking-change entry with the two-line C migration.
- [x] 4.2 `docs/CURRENT_STATUS.md` ABI policy text.
- [x] 4.3 Spec delta archived into `openspec/specs/engine-bindings/spec.md`.
