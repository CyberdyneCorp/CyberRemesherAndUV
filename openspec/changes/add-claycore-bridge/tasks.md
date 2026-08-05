# Tasks: add-claycore-bridge

- [ ] 1. Handoff format spec (versioned PLY/GLB profile + buffer layout),
       agreed with ClayCore's export-profile change — one shared document,
       two implementations
- [ ] 2. Handoff reader → Target (file + in-memory buffer), version gating,
       typed errors
- [ ] 3. `FieldEvaluator` interface + evaluator-backed sampling in the bake
       dispatcher (normal, AO, curvature); fallback parity tests vs raycast
- [ ] 4. Conform operation (topology-preserving re-snap over the snapper,
       max/RMS deviation report, threshold flagging)
- [ ] 5. CLI: handoff Target input (path/stdin), report coverage; end-to-end
       demo `clay export --for-retopo | cyber remesh --bake ...`
- [ ] 6. Bindings reachability + parity entries
- [ ] 7. Docs (handoff format doc committed to both repos) + CHANGELOG
