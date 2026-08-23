# Proposal: harden-untrusted-input-and-release-gates

## Why

Four adversarial hardening rounds (`fc5ccf6`, `7167729`, `ed9afcb`, `3b832ec`)
closed 89 defects, and several of them changed behaviour the specs describe. The
specs now state things that are no longer true, and — more dangerously — they are
silent about guarantees the code has started making, so nothing stops a later
change from taking them away again.

Six divergences, each with the defect that produced it:

1. **`application-shell` promises autosave persists "mesh, UVs, cage, parameters,
   history".** `Document::save` dropped every UV, vertex colour and feature-edge
   tag; a save/load round trip returned bare positions and face indices with
   `CYBER_OK` throughout. It also re-keyed soft-selection slots onto the wrong
   vertices once the id space had a hole, and `cyber_document_save_file` reported
   success for a write that never reached the device. The spec's promise is now
   met, by sections 6/7 of the container — and the *rules* that keep it met (the
   compacted-order convention, the deliberately unmoved format version) are
   nowhere written down.
2. **`remeshing-parameters` says no parameter is inert.** `sharpEdgeDegrees` was
   dead across the entire C ABI and Python: `cyber_remesh` built the default
   quad-cover extractor without passing it, so the extractor ran at its factory
   default of 40° and the ABI's own documented default of 90 could never take
   effect. The CLI honoured it, which is exactly why the "exposed equals
   implemented" scenario passed a source-level reading while the shipped ABI
   failed it. The same table still lists `quadMethod` with a `field-aligned`
   default and two values; the shipped default has been `quad-cover` since
   `2026-07-22-quadcover-default-and-auto-uv-atlas` and there are four.
3. **`remeshing-pipeline`'s isotropic stage had no termination guarantee.** A
   radius-1 sphere at world (5e5, 5e5, 5e5) — a centimetre-unit world, a CAD part
   in site coordinates — grew without bound until `bad_alloc` or the OOM killer.
4. **`mesh-io`'s "loud failure" requirement never said what happens on *hostile*
   input.** Four decoders could be made to read outside their buffer, hang, or
   commit gigabytes from a few hundred bytes, all from public entry points. And
   the writers reported success on a buffered write, so a full disk looked like a
   written file.
5. **`network-bridge` said malformed messages must not crash the application.**
   A single non-UTF-8 byte terminated the host *process* through an unguarded
   `dump()` in the error path, and the client half trusted the server the way the
   server had trusted the client.
6. **`compute-acceleration`'s parity requirement was met on paper and not in
   fact.** `test_gpu_parity` compared the CPU backend against itself in every
   configuration CI builds, and its generator could only emit square matrices —
   which is why all three GPU backends sizing `spmvCsr` by row count survived. The
   spec also promised nothing about a GPU backend not *degrading* CPU-side work,
   which is what made "every GPU backend runs `parallelFor` inline" invisible.

`build-and-packaging` and `engine-bindings` are the two that were simply behind
the tree: the release lane published binaries without running a test, the wheel
carried no native library, three compiled-in dependencies were absent from the
licence gate, and the C ABI's exported-symbol surface (now restricted on all
three platforms) had no requirement at all. Meanwhile `engine-bindings` described
a binding-parity gate as if it existed; since `f5c23fb` it does.

## What Changes

Spec-truth change. No engine work: every behaviour described here is already
implemented and tested in the tree, and this makes the specs say so.

- **mesh-io** — MODIFIED: hostile input is bounded and typed, not merely
  "unsupported"; a write is not successful until the bytes reach the file.
- **application-shell** — MODIFIED: the document container persists mesh
  attributes and feature-edge tags and rebases id-keyed annotations onto the
  serialized numbering; the format version stays put for an added section;
  hosting the engine inside another process leaves that process's handlers,
  streams and locale alone.
- **remeshing-parameters** — MODIFIED: the canonical table states the real
  `quadMethod` set and default; "no inert parameters" is stated as a per-entry-
  point guarantee (the ABI is an entry point) rather than a source-level one.
- **remeshing-pipeline** — MODIFIED: the isotropic stage terminates on any finite
  input, including coordinates where the float grid cannot express the target.
- **network-bridge** — MODIFIED: a hostile or malformed message can cost the
  connection but never the host process; the size ceiling binds both peers.
- **compute-acceleration** — MODIFIED: backend override includes the
  `CYBER_BACKEND` environment escape hatch; parity tests must run against every
  compiled-in backend over non-square shapes and fail rather than pass vacuously;
  selecting a GPU must not serialize the library's CPU-side parallelism, and
  repeated queries against one BVH must not re-upload it.
- **build-and-packaging** — MODIFIED: no artifact is published without the suite
  running; the licence gate covers everything compiled into a shipped binary, not
  just the manifest; packages carry the runtime dependencies their build gave
  them (including the Python wheel's native library); the static-analysis
  requirement matches what actually gates a merge.
- **engine-bindings** — MODIFIED: the shared library exports only its `cyber_*`
  ABI on every platform; the parity gate is named rather than assumed.

## Capabilities

- `mesh-io` — MODIFIED: untrusted-input and write-durability semantics.
- `application-shell` — MODIFIED: document persistence and host-process
  citizenship.
- `remeshing-parameters` — MODIFIED: canonical table corrected; inertness stated
  per entry point.
- `remeshing-pipeline` — MODIFIED: isotropic termination guarantee.
- `network-bridge` — MODIFIED: hostile-input containment.
- `compute-acceleration` — MODIFIED: selection override, honest parity, no
  GPU-induced CPU regression.
- `build-and-packaging` — MODIFIED: release gates, licence coverage, package
  completeness, real analysis gates.
- `engine-bindings` — MODIFIED: exported symbol surface; parity gate named.

## Impact

- A consumer reading these specs as a contract now gets the guarantees the code
  actually makes. Two of the previous texts would fail a conformance check
  against the shipped binary (`sharpEdgeDegrees`, autosave persisting UVs).
- The guarantees the hardening bought — bounded decoding, a host process left
  alone, honest parity — become requirements, so a future change that removes one
  fails review rather than passing unnoticed.
- Two known gaps are recorded rather than papered over: Metal parity is not
  reproduced on this project's hardware, and compute-backend selection is exposed
  by the C ABI but not yet by the Python or Swift bindings (`engine-bindings`
  keeps its unmodified requirement that it should be, so the gap stays visible).
