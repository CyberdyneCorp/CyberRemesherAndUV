## 1. Map catalogue in the bake module

- [x] 1.1 `cyber::bake::MapInfo` and a `constexpr` catalogue of every producible map: map,
      stable name, channel count, encoding basis under default parameters, colour space,
      and whether a field evaluator alone can produce it.
- [x] 1.2 `mapCatalog()`, `findMap(BakeMap)`, `findMap(std::string_view)` and
      `mapCatalogNames()`; `channelsFor()` and `fieldSupports()` in `bake.cpp` read the
      catalogue instead of their own switches.
- [x] 1.3 A compile-time check that the catalogue covers every `BakeMap` enumerator, so a
      map added later cannot be advertised as missing.

## 2. C ABI (ABI 1.22, additive)

- [x] 2.1 `CyberBakeProviderMap` with `structSize`, and the queries
      `cyber_bake_provider_map_count`, `cyber_bake_provider_map_at`,
      `cyber_bake_provider_find_map`, `cyber_bake_provider_map_list`.
- [x] 2.2 `CyberBakeProviderRequest` and `CyberBakeProviderResult`, both with `structSize`.
      The field refusal quotes the narrowed set from `cyber_bake_provider_map_list(1)`
      rather than from a second entry point of its own.
- [x] 2.3 `cyber_bake_provider_bake`: the same parameter validation `cyber_bake` applies
      (stricter on a non-positive width or height, never laxer), the
      texel ceiling, the optional `CyberFieldEvaluator`, the progress/cancel adapters, the
      caller-owned pixel and id buffers, and the result record.
- [x] 2.4 The sizing path (`pixels == NULL`): validate everything, report sizes, cast no ray.
- [x] 2.5 The refusals: unknown map code, map outside the field-producible set, short pixel
      buffer, descriptor size below the v1 layout — each naming what was asked for and
      what is available. The id table instead follows the repository's two-call
      convention (fill what fits, report the total), because its size is only knowable
      after the bake while the pixel count is knowable from the query before it.
- [x] 2.6 Cancellation: `CYBER_ERR_CANCELLED` with the caller's buffers untouched.
- [x] 2.7 `CYBER_ABI_VERSION_MINOR` 22, `capi/abi/cyber_capi-1.22.json` generated, and the
      manifest gate pinned to it.

## 3. Other entry points

- [x] 3.1 CLI: `--list-bake-maps` printing the advertised set straight from the catalogue,
      with a test pinning `--bake`'s documented vocabulary to those same names (the
      `--bake` parser already resolves through mesh-io's shared preset names, so it needs
      no second table).
- [x] 3.2 Python: `BakeProviderMap`, `bake_provider_maps()`, `find_bake_provider_map()` and
      `bake_provider_bake()` returning pixels plus a `BakeProviderResult`, with progress and
      cancellation callables.
- [x] 3.3 Swift: the same surface, and both binding-parity gates green.

## 4. Example

- [x] 4.1 `examples/26_bake_provider.py`: enumerate the advertised maps, size a request,
      bake maps of more than one encoding basis, print every metadata field, show the
      refusal naming an unproducible map, and show a cancelled request returning no pixels.
- [x] 4.2 Registered with the example runner and CTest like the other examples.

## 5. Tests

- [x] 5.1 Catalogue: every `BakeMap` advertised exactly once, and every row PINNED by hand
      — channel count, basis, colour space, field capability. `bake()` now reads the
      catalogue for its channel count and its field support, so comparing a bake against
      the table proves nothing; the hand-written copy is the independent statement. The
      cases that remain behavioural check what the table does not decide: the basis a bake
      reports, the colour space preset parsing defaults a map to, and that every row does
      name a map that really bakes at the size it states.
- [x] 5.2 Enumerate-and-request: every advertised map baked through the provider, each
      filling the caller's buffer with the advertised channel count and reporting a basis.
- [x] 5.3 The sizing call reports the right size, writes no pixel, and refuses an invalid
      request without baking.
- [x] 5.4 A short pixel buffer is refused with both capacities named and the buffer
      untouched.
- [x] 5.5 An unknown map code is refused naming the code and listing the advertised set.
- [x] 5.6 With a field evaluator and no Target, a field-capable map succeeds and a
      Target-only map is refused naming the map and the field-producible set.
- [x] 5.7 Cancellation returns `CYBER_ERR_CANCELLED` promptly and leaves a poisoned pixel
      buffer byte-for-byte unchanged.
- [x] 5.8 Progress is reported more than once for a ray-traced map.
- [x] 5.9 An id map's table arrives with the pixels and a picked colour resolves to exactly
      one row at zero tolerance.
- [x] 5.10 Descriptor sizing: a larger `structSize` is served, a smaller one is refused
      naming both sizes, and a longer result descriptor's surplus bytes come back
      untouched — the only direction that exists while 1.22 is the first layout.
- [x] 5.11 The provider takes the shared bake path: the same map requested through
      `cyber_bake` and through the provider is identical texel for texel, and the texel
      ceiling refuses a provider request exactly as it refuses a `cyber_bake` one.
- [x] 5.12 CLI: `--list-bake-maps` prints the advertised set, without duplicates, and every
      printed name is one `--bake` documents.
- [x] 5.13 Python binding test over the provider surface, registered with CTest.
- [x] 5.14 A short id table: the rows that fit are filled, the total is still reported, and
      no byte past the stated capacity is touched (poisoned rows beyond it are checked).
      Capacity 0 with a non-NULL buffer writes nothing, and asking again with the reported
      total returns the whole table.
- [x] 5.15 Every advertised map states a colour space, and only the colour map is sRGB —
      asserted at the C ABI, and tied in the catalogue tests to the colour space preset
      parsing defaults that same map to, so the two statements of the rule cannot drift.
- [x] 5.16 The projection cage reaches the bake: a Target sunk below the EditMesh is found
      by a cage long enough to reach it and missed by one that is not, through the provider
      and again through the Swift binding.
- [x] 5.17 Swift runtime test over the provider surface (`BakeProviderTests`): the
      advertised set, the provider's pixels against `Mesh.bake`'s texel for texel, the
      cage, repeated progress, an observed cancellation, an id table, and a refusal naming
      an unproducible map. Compiling the package never proved the marshalling.

## 6. Documentation

- [x] 6.1 CHANGELOG under `## [Unreleased]`.
- [x] 6.2 README: the provider surface in the bake documentation.
- [x] 6.3 `openspec archive add-bake-provider-capi --yes` once every task above is resolved.
