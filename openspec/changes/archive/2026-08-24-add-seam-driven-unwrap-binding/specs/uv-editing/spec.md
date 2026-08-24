# uv-editing — unwrap along caller-supplied seams

Delta for `add-seam-driven-unwrap-binding`.

## ADDED Requirements

### Requirement: Unwrap along caller-supplied seams
The automatic atlas SHALL accept a caller-supplied seam set and use it as the
cut instead of computing its own. When one is supplied the chart-growth and
chart-merge stages SHALL NOT run, because the seams already define the charts;
every later stage — island computation, conformal unwrap with the planar
fallback, chart re-orientation, packing and the reported metrics — SHALL be the
same code path the automatic atlas uses, so a seam-driven unwrap is reported in
the same terms as an automatic one.

A run given no seam set SHALL be unchanged, byte for byte, from the automatic
atlas as it behaves today.

#### Scenario: Marked seams define the charts
- **WHEN** a caller marks a seam set that cuts the mesh into a known number of islands and unwraps along it
- **THEN** the result SHALL report that many charts, and the charts SHALL be split exactly at the marked edges rather than at edges the engine chose

#### Scenario: An empty seam set is one chart
- **WHEN** a caller unwraps along a seam set with no edges marked on a closed mesh
- **THEN** the whole mesh SHALL unwrap as a single chart rather than being auto-seamed

#### Scenario: Chart growth options are inert
- **WHEN** a caller supplies a seam set together with chart-angle or chart-merge options
- **THEN** the charts SHALL follow the seams alone, and the result SHALL be identical to the same call with those options at any other value

### Requirement: Sewing islands back together is reachable
Sewing SHALL be reachable from the C ABI and the language bindings, not only
from C++, so that the seam set a binding consumer edits can be both cut and
sewn. Sewing merges two islands by removing the seam edges that separate them
and welding the corners across each removed edge.

#### Scenario: Sewing removes the cut
- **WHEN** a caller sews an edge that a previous unwrap had cut
- **THEN** that edge SHALL no longer be a seam, and a subsequent unwrap SHALL treat the two islands it separated as one
