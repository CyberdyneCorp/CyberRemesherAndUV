## Context

See proposal.md — Why. The relevant state of the tree:

- `BakeMap` values 0..10 are fixed; new ones are appended so the C ABI stays additive.
- The rasterized path already casts one cage ray per texel and shades from the hit
  (`shadeRasterTexel`), point-sampled with no anti-aliasing, overlaps resolved by
  last-write-wins. That is exactly the write discipline an id map needs, so an id map
  costs one more `case`, not a new pass.
- `BakeEncoding` already travels with the image through `BakeResult`, `BundleFile`, the
  C ABI and the CLI report. Anything hung on it reaches every entry point for free.
- The PNG writer (`writePngTonemapped` → `writePng`) clamps to `[0,1]`, rounds to 8-bit
  and deflates with STORED blocks and filter 0. It is lossless; the only lossy step is
  the float→byte quantisation, which is exact for values already on the 8-bit lattice.
- The repository already fixes `material_id` and `group_id` as face-domain int32
  semantic columns (`Mesh::tagFeatureEdges`, `collectSemanticBoundaryRequests`,
  `symmetry_layout`). No loader in this tree writes either of them today.
- `Mesh::islands()` returns face-connected components seeded in ascending face order,
  each sorted, with no unordered container anywhere — it is reproducible by
  construction.

## Goals / Non-Goals

**Goals:**
- A colour that is a pure function of the id, provably identical across toolchains.
- A written file that survives an exact, zero-tolerance comparison.
- The id→colour table reachable wherever the image is, with no new plumbing per entry
  point.

**Non-Goals:**
- Material or object *names*. This engine carries integer ids; the host owns the name
  table. The bake reports `(source, id) → colour`, which is the join key a host needs.
- Field-evaluator support. Like `Color`, `Position` and `Displacement`, an id map
  describes the Target MESH; `fieldSupports()` already excludes that class.
- UV border dilation of the new maps (issue #90, for every map at once). Note that
  dilation is the one post-process that *must* be nearest-neighbour for these maps;
  that constraint belongs to #90 and is called out in its own spec requirement there.
- Packing ids into a channel of another texture (CyberTexel's `texture-export`).

## Decisions

### 1. Hash, not palette — and an INTEGER hash, not ArmorPaint's

Issue #88 offers two options and asks that the choice be recorded. We take the hash.

A palette needs a documented assignment order, which means an *ordinal* — "the nth
distinct id gets the nth colour" — and an ordinal is state that has to be derived by
traversing something. That is precisely the shape of bug this repository has already
shipped (solver output differing between libc++ and libstdc++ through hash iteration
order). It also wraps once the id count exceeds the palette, at which point two ids
share a colour with no diagnostic. A hash of the id has no order, no state and no
bound.

We do **not** copy ArmorPaint's `frac(sin(dot(id, ...)) * 43758.5453)`
(`paint/sources/render/make_bake.c:78-93`). `sin` is not correctly rounded, and
implementations differ in the last ulp between libm versions, GPUs and CPUs; multiplying
by 43758.5453 and taking the fraction amplifies a 1-ulp disagreement into a completely
different colour. A float hash is exactly the wrong tool for a value that must be
bit-identical everywhere. We reuse `mixBits()`, the Wang-style integer avalanche already
in `bake.cpp` behind the per-texel AO rotation: defined bit-for-bit by C++'s unsigned
arithmetic, with no floating point anywhere on the path from id to byte.

The avalanche also answers the contrast objection the issue raises against hashing:
consecutive ids differ in about half their output bits, so `0, 1, 2, …` — the realistic
id sequence — produce maximally unrelated colours. What a hash genuinely cannot do is
guarantee contrast between two ids that happen to be *spatially* adjacent, which is not
knowable at assignment time under either scheme.

### 2. Channels lifted into `[64, 255]`, so black can be reserved

Each of the three bytes is taken from the avalanche and mapped `64 + (b * 3) / 4`. Two
things fall out: every assigned colour is visible on a dark background rather than
occasionally near-black, and `(0,0,0)` cannot be produced, so it is available as an
unambiguous "no id" for uncovered texels and cage misses. Reserving a sentinel that the
generator can still emit would be a bug that appears only for particular ids.

The cost is the colour space shrinking from 2^24 to 192^3 ≈ 7.1M. Collisions are a
birthday problem either way; the reported table, not the pixel, is the authority on what
an id's colour is, and a consumer resolving a picked colour looks it up there.

### 3. A cage miss writes "no id", it does not fall back to the low-poly

Every other map falls back to the EditMesh's own surface where the cage ray misses
(`shadeRasterTexel`'s `valid` branch). An id map cannot: the EditMesh is a remesh and
carries no material or object column of its own, so the only fallback available would be
to invent an id. Writing the reserved black is the honest answer and it is what the
padding already is, which keeps the padding continuous with the covered texels at a
chart border — the same argument `neutralPadding()` makes for every other map.

### 4. The table rides `BakeEncoding`, not a parallel channel

`BakeEncoding` is already defined as "what the numbers in this image MEAN", and it
already flows `bake() → BakeResult → BundleFile → CyberImageEncoding / the CLI report`.
Two members (`idSource`, `idColors`) on it make the table reach every entry point with
no new wiring in `bundle.cpp` at all. The C mirror `CyberImageEncoding` stays a flat POD
— a vector cannot live in it — so the table is read through separate accessors
(`cyber_image_id_color_count` / `_id_color` / `_id_source`, and the three bundle
equivalents), which is also how the ABI stays additive.

The table lists every distinct id on the Target's live faces, not only the ids that
happen to be visible in the layout. A consumer needs the whole key to resolve a colour;
which ids reached a texel is a property of the UV layout, not of the id assignment.

### 5. Object id resolution order: `object_id`, `group_id`, then components

`material_id` and `group_id` are this tree's established semantic columns, but nothing in
it *writes* them — the OBJ and glTF loaders produce neither. Without a fallback, an
object ID map baked from the CLI would be one flat colour on every real asset, which is
a feature that ships broken. `Mesh::islands()` is the right fallback because a
multi-part asset merged into one mesh carries its parts as disconnected components and
nowhere else, and because it is deterministic by construction (ascending face seed, no
unordered container). `object_id` is accepted ahead of `group_id` because that is the
name a host will reach for; `group_id` is kept because it is what the rest of this tree
already reads.

### 6. sRGB on an id map is refused, not applied

`writeMap()` currently honours `ColorSpace::Srgb` for any map. `linearToSrgb` is a
non-linear remap: applied to an id colour it produces a different byte, so the file no
longer matches the reported table and the whole point of the map is gone. It is refused
with a warning, exactly as the bundle already refuses sRGB into an EXR — the precedent
for "a colour-space request that would corrupt the data is reported, never applied
silently in either direction".

## Risks / Trade-offs

- **Colour collisions between distinct ids.** 192^3 values, so two ids collide with
  probability ~50% around 3.1k ids, and ~1% around 380. → The reported table is the
  authority and names both ids; a consumer that needs a guaranteed-unique key reads the
  table, and a future palette mode can be added behind a parameter without moving the
  default. Recorded here rather than hidden because it is the accepted cost of a
  stateless assignment.
- **A downstream mip/dilate/resize silently ruins the map.** Nothing in this repository
  does any of those to a bake output today, and the spec now states the requirement, but
  it is not enforceable from here. → `EncodingBasis::IdColor` in the recorded basis is
  the machine-readable flag a consumer can branch on, and issue #90's dilation will be
  written against it.
- **The component fallback changes when the Target's connectivity changes.** Welding two
  parts together renumbers every component after them. → Documented as a *fallback*; a
  host that needs stability across edits declares an `object_id` column, which the
  fallback defers to. The declared-column-wins scenario pins that.
- **`islands()` costs a traversal of the Target.** It is linear in faces and runs once
  per bake, against a per-texel cage ray each of which already costs a BVH descent. →
  Only computed when an object ID map is requested and no column declared one.
