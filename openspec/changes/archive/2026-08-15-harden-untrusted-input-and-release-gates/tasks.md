# Tasks: harden-untrusted-input-and-release-gates

Spec-truth and documentation change. The engine work landed in `fc5ccf6`,
`7167729`, `ed9afcb` and `3b832ec`; these tasks are about making the specs and
the consumer-facing documentation state what the tree now does.

## 1. Establish what actually changed

- [x] Read the four hardening commits and separate the fixes that changed
      documented behaviour from the ones that only removed defects.
- [x] Confirm each behaviour change against the source rather than the commit
      message: the extractor's `featureDegrees = 40` default that
      `sharpEdgeDegrees` never displaced
      (`quadcover_extractor.hpp:234`, `capi/src/capi.cpp:352`), the document
      container's sections 6/7 and the unmoved `kFormatVersion`
      (`src/app/include/cyber/app/document.hpp:26-66`), the isotropic
      coordinate-resolution floor and face budget
      (`src/core/src/isotropic.cpp:290-330,646-676`), the `CYBER_BACKEND`
      override (`src/accel/src/registry.cpp:58-79`), the export policy per
      platform (`capi/CMakeLists.txt:86-114`), and the close-checked writes
      (`src/imageio/src/detail.hpp:114-128`, `capi/src/capi.cpp:3062`).

## 2. CHANGELOG

- [x] Add the hardening work to `## Unreleased` as its own entry, grouped so a
      reader can find what affects them: behaviour changes first, then memory
      safety and untrusted input, API and contract corrections, the network
      bridge, compute backends, performance, packaging/CI/licensing, and what
      was verified.
- [x] Call out every consumer-visible behaviour change explicitly, each with the
      way to keep the old behaviour where one exists.
- [x] Leave the seven round-1 items already recorded under *Changed* / *Fixed*
      where they are, and say so, rather than duplicating them.

## 3. Specs

- [x] `mesh-io` — hostile input is bounded and typed; a write is not successful
      until the bytes reach the file.
- [x] `application-shell` — the document container persists attributes and
      feature-edge tags, rebases id-keyed annotations onto the serialized
      numbering, and keeps its format version put for an added section; the
      engine leaves a host process's handlers, streams and locale alone and
      retains nothing per call.
- [x] `remeshing-parameters` — the canonical table states the real `quadMethod`
      set and default; "no inert parameters" is a per-entry-point guarantee.
- [x] `remeshing-pipeline` — the isotropic stage terminates on any finite input,
      and the guards are inert at ordinary coordinates.
- [x] `network-bridge` — peer input can cost a connection, never the process;
      the size ceiling binds both peers.
- [x] `compute-acceleration` — `CYBER_BACKEND` override; parity cannot pass
      vacuously and covers rectangular operands; a GPU never serializes the
      CPU-side loops and a resident BVH is not re-uploaded.
- [x] `build-and-packaging` — release lane gated on the suite, licence audit
      scoped to what ships, packages carry their runtime libraries and the wheel
      carries its engine, and the analysis-gate requirement matches CI.
- [x] `engine-bindings` — the exported surface is the ABI on every platform, no
      exception crosses the boundary, and the parity gate is named.

## 4. Consumer documentation

- [x] `README.md` — correct the open-surface cleanup description (it is on by
      default with `CYBER_QC_NO_OPEN_CLEANUP` as the opt-out, and the graph
      simplification is skipped on open islands, not still merging their
      corners); add the compute-backend selection story, an environment-variable
      table, a supported-toolchain statement, the exported-symbol note for
      library consumers, and the packaging/release gates under Development.
- [x] `python/cyberremesh/README.md` — record the `sharp_edge_degrees`
      behaviour change with the way to restore the old output, note that typed
      statuses replaced escaping exceptions, and document backend selection via
      `CYBER_BACKEND` (it is not bound in the package).

## 5. Verify

- [x] `openspec validate harden-untrusted-input-and-release-gates --strict`.
- [x] `ctest -R release_gates` / `build_hygiene` still pass with the edited
      documentation (they assert the README's claims against the workflows).
- [x] `openspec archive harden-untrusted-input-and-release-gates` — merged into
      the eight capability specs; `openspec validate --all --strict` green.
