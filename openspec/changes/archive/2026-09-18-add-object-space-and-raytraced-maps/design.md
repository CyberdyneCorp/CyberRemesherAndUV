## `BakeMap::Position` is not redefined

`BakeMap::Position` already exists and means *the high-poly hit point in model
units*. Issue #87 asks for *the object-space position encoded `p * 0.5 + 0.5`
over the object's bounding box*. Those are the same SAMPLE with a different
ENCODING, so there were two options:

1. Change what `Position` writes.
2. Add a second map that samples identically and encodes differently.

(1) is out. `CYBER_BAKE_POSITION` is in the pinned ABI, in every built-in
preset's vocabulary and in both bindings, documented as "world space". A caller
reading model-unit coordinates would silently start getting `[0,1]` numbers with
no error anywhere — the failure mode this repository refuses everywhere else
(`paramsUsable`: "substituting a default would hide the caller's bug"). So (2):
`ObjectPosition`, with `Position` untouched.

There is one thing to be honest about: this engine has a single model space.
There is no object transform, so "world space" and "object space" are the same
space here. The real difference between the two maps is the encoding, and that
is exactly why the encoding has to be recorded (below) rather than implied by
the map's name.

## The encoding basis travels with the image

An object-space position map is `(p - min) / (max - min)`. Without `min` and
`max` a consumer cannot recover a single coordinate, and the map is a picture of
some numbers. The same is true of a thickness map multiplied by a scale factor,
and of any direction map whose up axis the producer chose.

So every bake now fills `BakeResult::encoding`, for every map, not only the new
ones:

| basis            | meaning                                            |
| ---------------- | -------------------------------------------------- |
| `None`           | raw values (AO, colour, curvature, cavity)          |
| `TangentNormal`  | unit direction in the texel's tangent frame, `v*0.5+0.5` |
| `ObjectNormal`   | unit direction in object space (`upAxis`), `v*0.5+0.5` |
| `ObjectBounds`   | object-space position rescaled over `[boundsMin, boundsMax]` |
| `Distance`       | a length in model units, multiplied by `scale`      |

It is attached to the IMAGE rather than returned beside it because the image is
what a host keeps, saves and hands on.

## The bounding box is the union of both meshes

`ObjectPosition` samples the high-poly at the cage hit, and falls back to the
low-poly's own surface point where the cage misses — exactly as `Position` does.
So a box that covered only one of the two meshes would clamp real texels. The
basis is therefore the union of the live-vertex bounds of both meshes, which is
the smallest box guaranteed to contain every value the map can write. A zero-
extent axis (a flat plate) encodes to 0.5 on that axis rather than dividing by
zero.

The box is computed IN the requested up-axis convention, so `boundsMin` and
`boundsMax` are directly usable for decoding without the consumer re-deriving
the swizzle.

## "Rasterization bake, no rays" still casts the cage ray

The issue describes the object-space normal and position as rasterization bakes
with no rays, and its acceptance list requires every new map to honour the
existing cage. In ArmorPaint those two maps are single-mesh bakes, so there is
nothing to project. Here every map is a high-to-low projection, and the cage is
what decides WHICH high-poly surface a texel speaks for.

Resolution: "no rays" is read as "no hemisphere ray budget" — the axis that
decides cost, progress and cancellation. The object-space maps cast the SAME
single cage projection ray `Normal`, `Displacement`, `Position`, `Color`,
`Curvature` and `Cavity` already cast, and fall back to the low-poly frame when
it misses. They cost one ray per texel, not `aoSamples`.

## Thickness: what a ray that escapes means

Thickness fires the AO hemisphere about the INVERTED normal and records the
distance to the first BACK-FACING hit — the point where the ray leaves the
solid. A ray that hits nothing within `aoRadius`, or that hits a FRONT face,
never was inside material, and contributes 0 rather than the radius.

That convention is what makes the trap in issue #87 come out right: a thin
double-sided surface has no interior, its rays escape, and the map reads near
zero rather than reporting some nominal plate thickness. The alternative
("a miss is maximally thick") would paint an open sheet solid white.

ArmorPaint doubles the recorded distance. That doubling is not arbitrary — the
mean chord of a cosine-weighted hemisphere through a slab of thickness `d` is
`2d`, so the factor undoes the hemisphere's own averaging for the slab case —
but it is a CHOICE, and an inherited constant nobody can see is how a bake
becomes unreproducible. It is `BakeParams::thicknessScale`, default 2.0,
recorded in the encoding as `Distance.scale`.

Thickness is written in MODEL UNITS, like `Displacement`, not normalised into
`[0,1]`. Normalising by `aoRadius` would bury the radius inside the value and
make the map uninterpretable without the parameters that produced it.

## Bent normal has a stated space, not an assumed one

A bent normal is useful in two frames: TANGENT space, where it drops in beside
the tangent-space normal map a shader already samples, and OBJECT space, where a
smart mask can compare it against a world direction. Neither is obviously right,
so it is `BakeParams::bentNormalSpace`, default `Tangent` to match
`BakeMap::Normal`, and the choice is recorded in the encoding. The up axis
applies only in the object-space case; tangent space has no up axis.

## One hemisphere gather for three maps

AO, bent normal and thickness are the same loop: cosine-weighted Hammersley
directions with a per-texel Cranley-Patterson rotation, fired in one
`accel::raycast` batch from the cage projection onto the high-poly. They differ
only in what they accumulate — an occluded count, a sum of unoccluded
directions, a sum of back-facing depths — and in the axis the hemisphere is
built around.

So `gatherHemisphere()` does all three and the shading pass picks. This is what
makes "shares the AO baker's sampling, accumulation, progress reporting,
cancellation and cage" true by construction rather than by copy. Openness for
an AO bake is bit-identical to what it was: same origins, same directions, same
order, same divisor.

## Progress is serialised, not sampled

The texel loop is the bake's one parallel region and the existing test pins that
it is handed to `parallelFor` exactly once, spanning every texel. Progress
therefore has to be reported FROM worker threads. `ProgressSink` merges values
monotonically but hands the host callback straight through, and a host callback
has no thread-safety contract, so the bake serialises the call behind its own
mutex and only reports on a 1% step. One `parallelFor` range, as before.
