## Context

See proposal.md — Why. The relevant state of the tree:

- `rasterize()` already returns exactly the covered texels, each carrying its pixel
  coordinate. That list IS the coverage mask; padding needs nothing the bake does not
  already compute, and in particular no second rasterisation pass.
- `bake()` pre-fills the whole image with `neutralPadding(map)` — a per-map neutral
  background (white for AO, mid-gray for curvature, black for the id maps). That is
  the *background*, not padding in this change's sense: it is what an untouched texel
  holds. The new stage overwrites a band of it.
- `BakeEncoding` already travels with the image through `BakeResult`, `BundleFile`,
  the C ABI and the CLI report, and already distinguishes a direction map
  (`TangentNormal`, `ObjectNormal`) from a scalar one and from an exact key
  (`IdColor`). The fill rule can therefore be chosen from a machine-readable flag
  rather than from a switch over map names that a future map would silently miss.
- Issue #88's design.md explicitly reserves the nearest-neighbour constraint for id
  maps to this change.
- Cross-toolchain determinism is a standing hazard here (libc++ vs libstdc++ hash
  order). Padding must therefore touch no unordered container and must not depend on
  the order texels are visited in.

## Goals / Non-Goals

**Goals:**
- A padded band that continues the surface, so a mip chain averages a continuation
  rather than a step.
- Channel semantics respected by construction: a direction stays a direction, a key
  stays exact.
- Bit-identical output on every toolchain, and independent of the order the padding
  loop happens to visit texels in.
- O(padded texels), not O(uncovered texels x radius x 8).

**Non-Goals:**
- Padding across a UDIM tile boundary, or into a neighbouring island's texels. That is
  issue #91's territory; here the band simply stops where it meets covered texels.
- Seam-aware padding that matches values across a UV seam's two sides. That needs the
  seam correspondence, which the bake does not carry.
- Padding the *mesh* export or the paint stage. CyberTexel dilates its own output.

## Decisions

### 1. Grow the band one ring at a time, rather than ray-marching from every empty texel

ArmorPaint's shader, read literally, searches outward in eight compass directions from
each empty texel until it finds a covered one. Two things push against transcribing
that directly:

- **It leaves holes.** A texel is only reachable if it lies exactly on one of eight
  rays from a covered texel. Off a small island, or off a diagonal border, many texels
  within the radius lie on none of them and stay unpadded — a band with gaps in it,
  which is worse than no band. ArmorPaint gets away with it because the pass is run
  repeatedly, each run feeding on the last; the iteration, not the ray, is what
  actually fills the band.
- **It is the wrong cost.** Searching `radius` steps in eight directions from every
  empty texel is O(uncovered x radius x 8) and pays most of that on texels nowhere
  near an island.

So we keep the iteration and drop the long ray. The band grows outward one ring per
pass: the candidates of pass *k* are the uncovered 8-neighbours of the texels covered
after pass *k-1*, which is exactly the set at Chebyshev distance *k* from the original
coverage. The "nearest covered texel in eight compass directions" is then found at
distance 1 by construction — a farther hit along some other compass direction could
never be nearer — so the search that remains is the eight immediate neighbours. After
`radius` passes the band is exactly `radius` texels wide, with no holes, in
O(padded texels).

A pass reads only the state from *before* it and writes only into texels that were
uncovered, so the result does not depend on the order candidates are visited in. The
candidate list is built in raster order from a raster-ordered frontier and no
unordered container appears anywhere on the path.

### 2. Continue the gradient with a two-tap linear extrapolation, averaged over the directions that hit

For a candidate texel `p` and a covered neighbour in direction `d`, let `v1` be the
value at `p + d` and `v2` the value at `p + 2d` when that texel is also covered. The
direction's contribution is `2*v1 - v2` — the linear continuation of the gradient the
two samples define. When `p + 2d` is not covered there is no gradient to continue and
the contribution is `v1`, which degrades to the copy behaviour only where the island
is one texel thick in that direction.

Contributions from all covered neighbours are averaged. Averaging rather than picking
the first direction in some fixed compass order matters for two reasons: a fixed order
biases the band toward one side of a straight border (three neighbours are covered
there, and taking only "N" tilts every padded texel the same way), and an average is
independent of the order the directions are enumerated in.

Compounding across passes is the point, not a defect: on a linear ramp, pass 1 writes
`e + s`, pass 2 reads that and writes `e + 2s`, and the band reproduces the ramp's
continuation exactly. That is what a hard copy cannot do.

### 3. Bound the continuation, but leave it room to run

Each ring reads the ring before it, so on a map that is not locally linear the
continuation compounds: a displacement several times the model's own range, an AO value
far outside `[0,1]`. Some bound is needed.

The obvious bound — clamp to `[min, max]` over the covered texels — is wrong, and
wrong in the direction that silently deletes the feature. A map whose values *ramp*
across its island reaches its own extreme AT the island border, which is exactly where
the band starts; clamping there flattens every continuation back into the repeated
edge value this stage exists to avoid. The scenario the issue asks for would fail, and
it would fail by producing precisely the artefact the issue names.

So each channel is clamped to `[min - span, max + span]`, `span = max - min` over the
covered texels. One range's width of slack is enough for any continuation a small
radius can produce over a real island, and it bounds a pathological map to three times
its own range. `span == 0` (a constant map) leaves no slack, which is correct: the
continuation of a constant is that constant.

The clamp is deliberately NOT applied to a renormalized direction map. A re-encoded
unit vector is already inside `[0,1]` on every channel, and clamping after normalizing
would shorten it again.

### 4. The fill rule comes from `EncodingBasis`, not from `BakeMap`

`EncodingBasis` is already the machine-readable statement of what the numbers mean,
and it is already filled for every map. Branching on it means:

- `IdColor` → nearest neighbour, exact copy. Not merely "do not average": do not
  touch the value at all. The nearest covered neighbour is chosen by a fixed compass
  order (N, NE, E, SE, S, SW, W, NW) — for an exact key a deterministic pick is
  required and an average is forbidden, which is the exact inverse of decision 2's
  reasoning, and the reason id maps cannot share that path.
- `TangentNormal` / `ObjectNormal` → extrapolate, then decode `v*2-1`, normalize,
  re-encode. A padded band of unnormalized vectors reads as a band of darkened,
  shortened normals in any shader that does not renormalize, which is most of them.
  Where the extrapolation collapses to a near-zero vector there is no direction left
  to preserve and the texel takes `+Z`: the neutral of both direction bases.
- everything else → extrapolate, no renormalization. Renormalizing a scalar map is
  meaningless — it would drive every padded texel to ±1 — and renormalizing a position
  map would move the point off the surface it describes.

A map type added later gets the right rule for free as long as it records an honest
basis, which it must do anyway.

### 5. Radius default of 8 texels

Bilinear sampling needs 1. Mip level *k* reaches `2^k`, so 8 texels covers the first
three mip levels, which is where seam bleed is visible; past that the whole island is
averaging into itself anyway. BC-family block compression works on 4x4 blocks, so 8 is
also two whole blocks of margin. Blender's bake margin defaults to 16 and Substance's
dilation to 16 at 4K; 8 at this repository's 512 default is the same fraction of the
image, and a host that wants Blender's number types it.

The radius is in TEXELS and not a fraction of the resolution, because what it has to
cover — a bilinear tap, a mip level, a compression block — is measured in texels. A
host that wants it to scale with resolution scales it itself, and the report says what
was applied.

### 6. Reported separately from the encoding, not folded into it

`BakeEncoding` answers "what do these numbers mean"; padding answers "which texels did
the bake write, and how". They are different questions, and a consumer that decodes an
object-space position needs the first whether or not it cares about the second. So
`BakeResult::padding` is its own record, with its own C accessor
(`cyber_image_padding`) mirroring `cyber_image_encoding`, and its own `padding` block
in the report's `outputs` entries.

`mode` is reported rather than inferred because "extrapolated" and "copied verbatim"
are different guarantees, and a consumer of an id map wants to see, in the report,
that the band was not interpolated.

## Risks / Trade-offs

- **Padding is on by default, so every bake's output changes.** That is the intent —
  an unpadded map is the bug — but it means existing expectations about background
  texels near an island now describe the pad band instead. The mitigation is that the
  band is bounded and reported: a test that needs the unpadded image sets
  `paddingRadius = 0`, which is also the documented way for a host to opt out.
- **Compounded extrapolation on a noisy map** amplifies the noise at the border. The
  range clamp bounds the amplitude, and the default radius bounds how many times it
  compounds.
- **Cost** is one pass over the covered texels plus eight neighbour probes per padded
  texel. At 4096² with radius 8 that is a few tens of millions of float operations
  against a bake that has already cast 16M cage rays; it is not measurable beside the
  raycast.
