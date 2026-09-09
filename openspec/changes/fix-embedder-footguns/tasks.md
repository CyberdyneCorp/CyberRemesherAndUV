# Tasks: fix-embedder-footguns

- [x] F1. `openness` replaces `occlusion` as the C++ and Python override point,
      with a Python shim that FORWARDS (never inverts) and warns. Inverting in
      the shim would flip a map that was already correct: the rename changed the
      name, not the quantity a pre-rename subclass was returning.
- [x] F2. `CYBER_BAKE_AO`'s comment states the polarity where the map is chosen,
      because that is where a host decides what to feed it.
- [x] F3. `cyber_mesh_topology_generation()`, bumped from `runEdit`'s existing
      `EditScope` so the counter cannot drift from the documented rules without
      the scope itself being wrong. Carried across a clone.
- [x] F4. `cyber_set_max_import_vertices()` / getter, threaded through
      `ImportOptions::maxVertices` and enforced once in `importMesh`.
- [x] F5. ABI 1.0 -> 1.1, with a test asserting a 1.0-compiled client is still
      served — the additive-only rule exercised rather than asserted.
- [x] F6. The header is a CMake configure dependency: `file(STRINGS)` reads at
      configure time, so without it an ABI bump left a stale SOVERSION and a
      stale library VERSION in an existing build tree, disagreeing silently.
- [x] F8. `cyber_seamless_solver()` (ABI 1.2). Which solver a build carries was
      reachable only from the CLI, so a host that embeds the library could not
      learn it -- and the fallback to the portable quadrangulator is silent and
      changes the output. Found by an embedder writing a type to report it and
      finding nothing could construct it.
- [ ] F7. Peak-bounded parsing for the formats that declare their counts.
      DEFERRED: the ceiling bounds the RESULT, which is the useful boundary
      (loading is the cheap half; the pipeline is where the cost is), and a
      peak bound is a per-format change to five importers.
