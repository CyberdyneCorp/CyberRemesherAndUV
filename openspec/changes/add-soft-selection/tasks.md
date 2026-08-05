# Tasks: add-soft-selection

- [ ] 1. Weight computation: line gradient (with 15° snap), sphere falloff,
       painted accumulation (+ subtract), stored per-vertex on EditMesh
- [ ] 2. Selection ops: clear/invert/expand/contract/smooth(1|5|10),
       save/load named slots persisted in the document
- [ ] 3. Weighted transform (T/R/S) + weighted relax with in-operation
       re-snap to the Target
- [ ] 4. C ABI + stroke route for painted mode; Python + Swift bindings;
       parity gate entries
- [ ] 5. Tests: ramp/saturation, zero-weight immobility, glue invariant,
       save/load round-trip
- [ ] 6. Docs + CHANGELOG
