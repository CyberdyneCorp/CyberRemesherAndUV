## Context

See proposal.md — Why. Three constraints from the code shape everything below.

1. **This engine has one model space.** `EncodingBasis::ObjectNormal` is documented as
   "unit direction in object space (`upAxis`)", and #87 recorded that "world" and "object"
   name the same space here. Nothing in `bake()` places the Target/EditMesh pair anywhere.
2. **Border padding reads the ENCODING BASIS, not the map's name** (`paddingModeFor`), and
   #90's repair added a second bound: a padded texel must also stay inside the value range
   the map's own encoding guarantees (`BakeEncoding::valueMin/valueMax`). A new map that
   declares an honest basis and an honest range gets the right band for free; one that
   does not gets a silently wrong one.
3. **`CyberBakeParams` is passed by pointer and never as an array**, but has no
   `structSize`; the three bake-provider descriptors do have one and are documented as the
   only structs here where appending is additive.

## Goals / Non-Goals

**Goals:**

- Ship a world-space direction map that is genuinely distinct from `object-normal`.
- Ship a UV density map with both normalizations and a degenerate case that cannot poison
  the relative one.
- Keep both maps on the shared bake path: same cage, texel ceiling, progress, cancellation
  and padding, with no map-specific exception anywhere.

**Non-Goals:**

- A world-space POSITION map. `position` and `object-position` already exist and a third
  position map is a separate decision; only #89's direction map is in scope.
- Making the placement transform affect any existing map. `position`, `object-position`
  and `object-normal` keep their current meaning exactly.
- UDIM. #91 owns it; this change states the constraint it must satisfy and nothing more.
- Component links. `surface-baking`'s "Component links and selective baking" requirement
  has NO implementation in `src/bake/` — `bake()` takes no component-link input at all.
  The honest claim for the two new maps is that they take the identical code path every
  existing map takes, and they will reach component links on the same terms as the rest
  once that path exists. No scenario here asserts otherwise.

## Decisions

### D1. Ship the map, and ship the transform that makes it real (option (a))

The three options were: (a) introduce a placement transform so the map is genuinely
distinct; (b) do not ship the map and correct the spec's implication that a transform
exists; (c) ship a differently-named map computing the same numbers.

(c) is rejected outright: a second name for bit-identical pixels is a map a consumer will
bake, store and ship twice, and the first person to diff them loses confidence in the
whole set.

(b) is honest but leaves #89's stated purpose — "it follows the object's placement
transform, which is the point" — unbuilt, and leaves CyberTexel's world-direction
generators with no input, which is the reason the epic exists.

So (a). `BakeParams::placement` is a **4x4 row-major object→world matrix**, default
identity. A 4x4 is what a host already holds (every DCC and every scene graph hands one
out), so nothing has to be decomposed on the way in. Only the upper-left 3x3 is read
today, because the only map that reads the placement is a DIRECTION map and a direction is
unaffected by translation; the translation is accepted and recorded so that a world-space
POSITION map, if one is ever added, does not need a second parameter with a different
shape.

Alternatives considered: a quaternion + translation (rejected — it cannot express the
scale a placement routinely carries, and the caller would have to decompose); a bare 3x3
(rejected — the host would have to slice its own matrix, and a future position map would
need a second parameter).

### D2. A normal is carried by the INVERSE TRANSPOSE, and identity is exact

`n_world = normalize(transpose(inverse(M3)) * n_object)`. A plain `M3 * n` is correct only
for a rotation; under non-uniform scale it shears the normal off the surface, which is the
classic bug and would make the map wrong exactly on the assets that need it (a placement
with a scale).

`toUpAxis` is applied **after** the placement, mirroring `object-normal`: the placement is
expressed in the engine's own space, and the up axis is a re-expression of the OUTPUT.

The identity case is special-cased to a byte-for-byte skip rather than allowed to fall out
of the arithmetic. `normalize()` of an already-unit vector is not the identity in float —
`length()` can return `0.99999994` — so without the skip "identity placement equals
`object-normal`" would be true only to a tolerance. Making it exact turns the equality
into something a test can assert at zero tolerance, and makes the statement "these two
maps differ by the transform and by nothing else" literally true.

A placement whose linear part is **singular** (determinant 0) has no inverse transpose:
there is no direction to carry. It is refused — the bake returns no image — under the
existing rule that an out-of-range parameter is refused rather than defaulted, and only
when the requested map actually reads it, matching `paramsUsable`'s existing policy that
"parameters the requested map never reads stay unchecked".

### D3. UV density is texels per unit of SURFACE AREA, measured per sub-triangle

`density = uvArea * width * height / surfaceArea`, where `uvArea` is the rasterized
sub-triangle's area in UV units (the unit square is the whole map) and `surfaceArea` is its
area in model units. `width * height` converts UV area into a texel count. The units are
**texels per square model unit**; a host wanting the linear "texels per metre" convention
takes the square root, which the spec says so nobody guesses.

The ratio `uvArea / surfaceArea` is resolution-independent, so the rasteriser computes and
stores THAT per texel and the shading pass multiplies by `width * height`. The rasteriser
is where the sub-triangle's two areas are already in hand; recovering them later would
mean carrying a face id and re-deriving them.

Alternative considered: deriving density from the cage hit on the Target. Rejected — UV
density is a property of the EditMesh's UV layout, not of the Target, and reading it off
the Target would make the map change when the Target changed, which is the opposite of
what it is for.

### D4. The degenerate sentinel is exactly 0, and it is unreachable as a measurement

A covered texel whose sub-triangle has no UV area or no surface area has no density: the
ratio is `0/x`, `x/0` or `0/0`. All three, plus a non-finite or underflowed result, take
the single sentinel **0**.

0 is safe as a sentinel for the same reason `(0,0,0)` is safe for an id map: no defined
density can take it. A texel exists only because its sub-triangle covered it, so a defined
density has a positive UV area over a positive surface area and is strictly positive. The
classification is written as `!(d > 0) || !isfinite(d) -> 0`, so a value that underflows to
zero is classified as undefined too, and the sentinel keeps its meaning.

It is also the map's background value, so an uncovered texel and an undefined texel read
the same — which is what a consumer wants: "no density here" either way.

Alternatives considered: NaN (rejected — it propagates through the mean, through padding
and through the PNG writer's clamp, which is precisely the poisoning #89 names); a negative
sentinel such as `-1` (rejected — it forces the map's value range to admit negatives, and
the padding band's own bound would then be free to go negative on a real density map).

### D5. The relative mean is the arithmetic mean of the DEFINED texels, taken from the image

Relative normalization divides every defined texel by the mean of the defined texels. The
mean is computed by one pass over the finished image in raster order, counting only values
`> 0`. That has three properties worth the choice:

- Uncovered texels hold the background, which is the same 0 the sentinel uses, so they fall
  out of the mean with no coverage set to consult.
- Overlapping texels (a UV layout where two faces land on one texel) are counted once,
  because the image holds the winner rather than every writer.
- Raster order over a `std::vector<float>` touches no unordered container, so the mean is
  identical on libc++ and libstdc++ — the cross-toolchain trap this tree has already been
  bitten by.

The mean is recorded in the encoding whichever mode was selected, so a relative map
converts back to absolute by multiplying, and an absolute map still tells a host what its
own average density is. When nothing is defined the mean is 0 and the map is left at the
sentinel everywhere rather than divided by zero.

The pass runs **before** border padding, so the band extrapolates the values that were
finally written rather than pre-normalization ones.

### D6. Two new encoding bases, because padding reads the basis

`EncodingBasis::WorldDirection` takes the `ExtrapolateUnit` rule (a direction map's band
must decode to unit directions) and the `[0,1]` range `n * 0.5 + 0.5` guarantees.

`EncodingBasis::UvDensity` takes the `Extrapolate` rule — a density is a scalar and
renormalizing it would replace the map's values with directions — and declares its range as
**`[0, +inf)`**: a density is never negative, and the ratio of texels to surface area has
no upper bound. That is deliberately not `[0,1]` and it is stated in the spec, because
#90's repair confines a padded band to the map's declared range and an honest declaration
is the only thing that makes it right.

Reusing `ObjectNormal` for the world map was rejected: it says "object space (`upAxis`)",
and the whole point of this change is that the two are no longer the same space.

### D7. C ABI: additive entry points, no change to any struct a caller already passes

`CyberImageEncoding` is **not** extended. It has no `structSize`, and `cyber_image_encoding`
WRITES into the caller's buffer; growing it would overrun a caller compiled against 1.22.
It is also embedded mid-struct in `CyberBakeProviderResult`, so growing it would shift
every member after it and break the descriptor-size promise.

Instead:

- a new `CyberImageDensity { normalization, mean }` with `cyber_image_density`, and
  `cyber_image_placement(image, float out[16])` — new structs and entry points used only by
  new calls, which is additive under this header's own rule;
- `CyberBakeProviderResult` gains `density` and `placement` **at the end**, which the
  documented DESCRIPTOR SIZES exception makes additive; its accepted floor is frozen at the
  1.22 layout with `offsetof(...idSource) + sizeof(const char*)` so a 1.22 caller is still
  served;
- `CyberBakeParams` gains `placement[16]` and `densityNormalization` at the end, following
  the precedent of the 0.8.0 (`upAxis`, `bentNormalSpace`, `thicknessScale`) and 0.9.0
  (`paddingRadius`) appends, which took the same shape and bumped the ABI MINOR. The
  consequence is the same one those appends carry: a caller compiled against an older
  header must re-initialise through `cyber_default_bake_params`. ABI 1.22 -> 1.23.

This last one is in tension with two pieces of prose in the header — the top-of-file rule
that "any change to a struct's size or field order — appending included" is MAJOR, and the
comment above `cyber_bake_field` explaining that it is a separate entry point because
"growing that struct would break every caller that allocates it". Both of the 0.8.0 and
0.9.0 appends already crossed that line and moved only the MINOR, and 0.9.0 is unreleased,
so this change follows the established practice rather than either bumping the MAJOR for
two fields or inventing a third way to pass a parameter. Reconciling the prose with the
practice — by giving `CyberBakeParams` a `structSize` the way the provider descriptors have
one, or by bumping the MAJOR once — is a decision worth taking deliberately and is out of
scope here.

### D8. UDIM is stated as a forward constraint, not implemented

#91 has not landed. The spec records what a UDIM-aware bake must preserve — a face's
density is computed against the resolution of the tile it lands in, so a set of tiles at
one resolution reads the same density as a single map at that resolution, and the relative
mean is taken over the whole set rather than per tile, or two tiles of different coverage
would each call themselves average. No scenario asserts UDIM behaviour, because there is
none to assert.

## Risks / Trade-offs

- **The world-direction map is a duplicate whenever the host leaves the placement at
  identity.** → That is by construction and is documented as the map's identity case, not
  hidden. The spec's scenarios pin both halves: identity equals `object-normal` exactly,
  and a non-identity rotation changes the output.
- **`CyberBakeParams` grows again without a `structSize`.** → Unchanged risk profile from
  the two previous appends in this same unreleased cycle; the ABI MINOR moves and
  `cyber_default_bake_params` remains the documented way to initialise it. Fixing the
  underlying pattern is a separate, larger decision.
- **A density map's values are unbounded above, so its padded band is bounded only by the
  compounding limit.** → Correct and intended: any upper bound would be a fiction, and the
  compounding limit already bounds runaway to three times the covered range.
- **`uv-density` ignores the Target although the shared path requires one.** → Accepted
  rather than special-cased. Exempting it from the Target requirement would be exactly the
  map-specific exception the spec forbids; the cost is one wasted cage ray per texel and
  the benefit is that every rule about the shared path stays true without an asterisk.
