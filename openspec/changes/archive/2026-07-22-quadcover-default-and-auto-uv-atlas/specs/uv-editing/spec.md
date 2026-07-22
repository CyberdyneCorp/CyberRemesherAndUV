# uv-editing (delta)

## ADDED Requirements

### Requirement: Automatic UV atlas
In addition to the interactive editor (hand-drawn seams, gesture unwrap, manual/automatic packing), the UV capability SHALL provide a non-interactive automatic atlas: given a mesh with no UVs and no hand-drawn seams, it SHALL seam the mesh into charts, unwrap each chart, pack them into the unit UV square, and write the per-corner UV attribute, reporting chart count and distortion so callers can gate on quality. The pipeline SHALL be deterministic and each stage SHALL be independently controllable.

#### Scenario: Mesh in, packed atlas out
- **WHEN** the automatic atlas runs on a mesh that carries no UVs
- **THEN** it SHALL grow charts by normal coherence, LSCM-unwrap each chart (falling back to a planar projection for a chart LSCM rejects), and pack the charts into the 0–1 UV square without overlaps, leaving every corner with a UV coordinate and returning chart count, maximum and RMS conformal (angle) distortion, flipped-chart count, and packed-area / texel-density readouts

#### Scenario: Chart merging trades seams against distortion under a bound
- **WHEN** chart merging is enabled
- **THEN** the atlas SHALL first merge adjacent charts whose union stays within one normal cone (fewer seams with no rise in distortion), then optionally merge further while the combined conformal-plus-area distortion of the LSCM-unwrapped union stays at or below a caller-set cap (folding developable regions together to cut the chart count), and SHALL NOT merge charts that would fold or exceed the cap

#### Scenario: Charts are re-oriented before packing
- **WHEN** chart re-orientation is enabled
- **THEN** each chart SHALL be rotated to its minimum-area bounding rectangle before packing (a similarity that leaves conformal distortion and flip state unchanged), so the packer wastes less space and texel density rises

#### Scenario: Conformal quality is preserved
- **WHEN** the automatic atlas unwraps normal-coherent charts on a remeshed quad mesh
- **THEN** the maximum per-chart conformal (angle) distortion SHALL stay low (angle-preserving charts) with no flipped charts, and the result SHALL be exposed for benchmarking against a reference unwrapper (e.g. xatlas) on chart count, distortion, and coverage
