## Why

Epic #86 makes this repository the baker for [CyberTexel](https://github.com/CyberdyneCorp/CyberTexel),
whose `mesh-maps` capability deliberately ships no baker. Issues #87, #88 and #90 landed
the maps: object-space normal/position, bent normal, thickness, material and object ID,
and the padded band every map now carries. What is missing is the seam itself — the way a
consumer *asks* for one of them (#93).

Today a consumer reaches the maps through `cyber_bake`, and that entry point cannot serve
this role:

- It **cannot be asked what it can produce.** A consumer has to hard-code the map list, so
  a build that grew or lost a map is indistinguishable from one that did not until a bake
  fails — or worse, does not.
- It **takes no progress or cancellation callbacks**, although `cyber::bake::bake()` has
  honoured both internally since the bake stage existed. A 4K bent-normal bake is minutes
  of work a host cannot interrupt or draw a bar for.
- It returns an **opaque handle the consumer must free**, then four more calls to read the
  pixels and the metadata. A consumer that owns its own texture memory copies twice.
- The metadata #87, #88 and #90 produce — up axis, encoding basis, normal green-channel
  convention, id-to-colour table, padding radius and fill rule — is reachable, but only
  by knowing which of `cyber_image_encoding` / `cyber_image_padding` /
  `cyber_image_id_source` / `cyber_image_id_color` to call.

The mirror-image precedent is already here: `pipeline-bridge`'s `CyberFieldEvaluator` is
three C callbacks a volumetric engine implements so `cyber_bake_field` can sample it. This
change is that pattern pointing the other way — the callbacks come from the consumer, the
maps go back to it, and neither repository links the other.

The behaviour that matters most is a refusal. CyberTexel's `mesh-maps` states that a map
it cannot obtain is reported **by name** rather than substituted with a neutral value,
because a smart material silently reading flat grey for curvature looks subtly wrong on a
new model instead of loudly broken.

## What Changes

- **A capability query.** `cyber_bake_provider_map_count` / `_map_at` / `_find_map` /
  `_map_list` describe the maps *this build* produces: the map code, a stable machine
  name matching `mesh-io`'s preset vocabulary (`normal`, `ao`, `object-position`, …), the
  channel count, the colour space, the encoding basis a bake of it reports, and whether a
  field evaluator alone can produce it. A consumer sizes its buffer and builds its UI from
  this, never from a hard-coded list.
- **A request, synchronous, with the consumer's callbacks.**
  `cyber_bake_provider_bake` takes one descriptor carrying the Target/EditMesh pair, the
  map, the bake parameters, an optional `CyberFieldEvaluator`, a `CyberProgressCb`, a
  `CyberCancelCb` and their shared `void* user`. Synchronous-with-a-callback rather than a
  polled handle: it matches `cyber_bake_field` and `cyber_export_bundle_write`, and a
  consumer that wants a job puts the call on its own thread.
- **Caller-owned buffers with two-call sizing.** The pixels are written into the
  consumer's `float*`; the id-to-colour table into its `CyberIdColor*`. A request with a
  null pixel buffer validates everything and reports the sizes without baking, so the
  sizing call costs no rays.
- **Result metadata alongside the pixels.** One descriptor carries width/height/channels,
  the encoding basis, the up axis, the normal green-channel convention, the object-space
  bounding box, the distance scale, the padding radius/mode/texels, the id source, the id
  table size and the covered-texel count — so nothing about the map has to be documented
  out of band.
- **Descriptors carry their own size.** `structSize` is the first member of all three
  descriptors, and the library reads and writes only what the caller's size covers. A
  member appended later keeps its documented default for an older caller, which makes
  these three structs the only ones in this ABI where appending is additive. They are
  passed one at a time by pointer, never as arrays, which is what makes that safe.
- **A map this build cannot produce is refused by name.** An unrecognised map code, or a
  map the attached field evaluator cannot answer, fails with `CYBER_ERR_INVALID_ARG`
  naming the map and listing the advertised set. No neutral image is ever returned.
- **Cancellation returns promptly with nothing written.** The status is
  `CYBER_ERR_CANCELLED`, and the consumer's pixel and id buffers are left exactly as they
  were — a partial band or a half-shaded map must never reach a consumer that cannot tell.
- **One map catalogue, shared.** The advertised set lives in `cyber::bake` and is what the
  C ABI, the CLI's `--bake` parser and its new `--list-bake-maps` all read, so the three
  cannot disagree.
- **Bound in Python and Swift**, and exercised by a headless example under `examples/`.
- ABI 1.22, additive: three new descriptors, four new queries, one new entry point.

## Capabilities

### Modified Capabilities

- `engine-bindings`: a new requirement fixes the provider surface — the capability query,
  the request, the two-call sizing, the self-describing descriptors, the refusal that
  names the map and the advertised set, the cancellation contract, and the rule that the
  advertised set and the map names the other entry points accept are one list.
- `pipeline-bridge`: a new requirement fixes the seam's direction and its independence —
  a consumer drives the provider with nothing but this repository's C ABI, and the
  provider accepts the same `CyberFieldEvaluator` the bridge already defines, restricting
  the producible set to the maps a field can answer and refusing the rest by name.

## Impact

- `src/bake/` — the map catalogue (`MapInfo`, `mapCatalog()`, `findMap()`), with
  `channelsFor` / `fieldSupports` reading it instead of their own switches.
- `capi/` — ABI 1.22 (`capi/abi/cyber_capi-1.22.json`), `CyberBakeProviderMap`,
  `CyberBakeProviderRequest`, `CyberBakeProviderResult`, `cyber_bake_provider_map_count`,
  `_map_at`, `_find_map`, `_map_list`, `cyber_bake_provider_bake`.
- `apps/cli/` — `--list-bake-maps`, and `--bake` parsed against the catalogue.
- `python/`, `swift/` — the provider surface and both binding-parity gates.
- `examples/26_bake_provider.py` (new), `tests/bake/test_map_catalog.cpp` (new),
  `tests/capi/test_capi_bake_provider.cpp` (new), `tests/cli`, `tests/packaging`,
  CHANGELOG, README.
