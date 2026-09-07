# Tasks: add-semantic-boundaries-and-symmetry-detection

Carried from `add-zremesher-retopology` (F1, F2, F4) with their recorded
reasoning intact. Ordered so the ABI decision, which gates the other two, comes
first.

## Milestone 1 — the missing input

- [ ] M1. A sibling ABI entry point carrying per-face group / material ids.
      `CyberRemeshParams` cannot grow fields without breaking compiled callers,
      so this is a new entry point, not an extension.
      Gate: a caller supplies group ids and reads them back unchanged; existing
      compiled callers keep working against the unchanged struct.
- [ ] M2. Decide whether the OBJ loader retains `g` / `usemtl`, and record the
      decision either way.
      Gate: the decision is written down in the spec, with its cost.

## Milestone 2 — boundaries reach the field

- [ ] M3. Tag edges where adjacent faces disagree in group or material as
      feature edges, through `projectGuideToPath` -> `setFeatureEdge`, applied
      after the dihedral re-tag so they cannot be demoted.
      Gate: a two-material cube keeps the material boundary as an edge loop.
- [ ] M4. Connect those boundaries to field pinning, the topology layout and
      the sizing field.
      Gate: the layout reports arcs along a material boundary that no dihedral
      angle would have produced.

## Milestone 3 — symmetry detection, report before apply

- [ ] M5. Detect a symmetry plane and REPORT it, without applying it.
      Matching is nearest-within-tolerance — never a quantized tolerance grid,
      which both collides distinct vertices and misses partners across a cell
      boundary.
      Gate: the corpus's symmetric models report their plane; the asymmetric
      ones report none, and nothing about the output changes.
- [ ] M6. Only once M5's threshold is calibrated on the corpus, allow detection
      to select the axis that forced symmetry then applies.
      Gate: no model whose detection is uncertain is silently mirrored.
