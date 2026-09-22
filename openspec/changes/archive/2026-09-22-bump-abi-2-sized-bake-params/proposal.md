## Why

v0.9.0 shipped C ABI **1.16**, where `CyberBakeParams` had 6 members and
`CyberBundleParams` 5. The mesh-map epic (#86) then appended 6 members to each
while moving only the ABI **minor**, to 1.24. The header's own rule says appending
to a struct is a major change, and the reason is concrete:

A C host compiled against 0.9.0 holds a 6-member `CyberBakeParams` on its stack.
Linked against today's `libcyber_capi.so.1` — which the soname promises it can be —
it calls `cyber_default_bake_params`, and the library writes all 12 members:
**84 bytes past the end of the caller's struct**. `cyber_bake` then reads those
bytes back as parameters. Throughout, `cyber_abi_check(1, 16)` returns `CYBER_OK`,
so the one mechanism meant to catch this certifies it instead.

Nothing has shipped with the appended layout yet, so this is the moment to fix it
properly rather than to paper over it.

## What Changes

- **BREAKING: C ABI 2.0.** `CYBER_ABI_VERSION_MAJOR` becomes 2 and the minor resets
  to 0. The soname becomes `libcyber_capi.so.2` (it is already derived from the ABI
  major). A binary compiled against any 1.x header fails loudly — it does not load,
  and `cyber_abi_check(1, x)` refuses it — instead of corrupting memory.
- **`CyberBakeParams` and `CyberBundleParams` carry `size_t structSize` as their
  first member**, the mechanism the bake-provider descriptors already use. The
  library reads, and `cyber_default_*_params` writes, only the bytes the caller's
  stated size covers; a member the caller's size does not cover takes its ENGINE
  DEFAULT. Appending to these two structs is therefore additive from 2.0 on, so a
  new bake option no longer needs another major.
- **`cyber_default_bake_params` and `cyber_default_bundle_params` return
  `CyberStatus`.** They need `structSize` set first, and a void initializer that
  silently does nothing when it is missing is a trap; they now refuse it by name.
- **The header states one rule for sized structs** — the three provider descriptors
  plus these two — replacing the "one documented exception" wording, and drops the
  "Appended in 0.8.0 / 0.9.0" member annotations, which were wrong (none of those
  members is in the v0.9.0 tag) and are meaningless at 2.0.
- Python and Swift set `structSize` themselves, so neither binding's public API
  changes. The Python loader looks for the `.2` soname.

## Capabilities

### New Capabilities
<!-- none -->

### Modified Capabilities
- `engine-bindings`: the C ABI's compatibility requirement is stated per major and
  gains a scenario for a client of an older major; a new requirement defines sized
  parameter structs.

## Impact

- **C/C++ consumers of the shared library must recompile** against the 2.0 header.
  Source changes: set `params.structSize = sizeof params` before
  `cyber_default_*_params`, and check its return value.
- Python wheels, the Swift package / XCFramework and the Rust crate each ship or
  build their own matching library, so their users see no change.
- `capi/abi/cyber_capi-2.0.json` becomes the pinned manifest; the 1.x manifests stay
  as history.
