# manual-retopology — soft selection

Delta for `add-soft-selection`.

## ADDED Requirements

### Requirement: Gradient region selection
The retopology stage SHALL compute per-vertex selection weights in [0,1] from
three region sources: a line gradient (anchor to end point, weight ramping
0→1 along the gradient and 1 beyond it, with optional angle snapping in 15°
increments), a sphere (center and radius with an easing falloff), and painted
strokes (weights accumulated under the brush, with a subtract mode).

#### Scenario: Line gradient ramps and saturates
- **WHEN** a line selection is made from anchor A to end B
- **THEN** vertices at A SHALL weigh 0, vertices at B SHALL weigh 1, vertices beyond B SHALL weigh 1, and the ramp between SHALL follow the falloff curve

#### Scenario: Painted weights accumulate
- **WHEN** overlapping paint strokes cover a region
- **THEN** weights SHALL accumulate toward 1 and a subtract stroke SHALL reduce them

### Requirement: Selection operations
Soft selections SHALL support clear, invert, expand, contract, and smoothing
by 1, 5, or 10 steps, and SHALL be savable to and loadable from named slots
persisted with the document.

#### Scenario: Smooth-by-10 softens the gradient
- **WHEN** a hard-edged painted selection is smoothed by 10
- **THEN** the weight transition SHALL widen without moving geometry

### Requirement: Weighted transform with surface glue
Translate, rotate and scale SHALL apply per-vertex scaled by selection weight,
and relax SHALL accept the weights. During and after a weighted transform the
affected vertices SHALL remain snapped to the Target surface; auto-snapping is
part of the operation and SHALL NOT require a separate pass.

#### Scenario: Taper by line gradient stays on the sculpt
- **WHEN** a line selection spans a limb and a scale transform is applied
- **THEN** the limb SHALL taper along the gradient and every affected vertex SHALL lie on the Target surface afterward

#### Scenario: Zero-weight vertices do not move
- **WHEN** any weighted transform runs
- **THEN** vertices with weight 0 SHALL be bit-identical to their pre-transform positions
