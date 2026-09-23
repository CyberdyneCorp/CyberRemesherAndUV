## Why

`surface-baking` caps output resolution at "at least 4096²", and every bake in this tree
allocates its whole output up front: a map is `width * height * channels` floats in one
buffer. A 16384² RGB map is ~3 GB in that shape, so the resolution CyberTexel's texture sets
actually use (`mesh-and-texture-sets`, up to 16384) is unreachable on a machine that cannot
hold the whole map at once — and a mesh map at a quarter of the document's resolution is a
soft mask driving a sharp material, the wrong failure for exactly the edge-wear and dirt
generators mesh maps exist for (#92, epic #86).

The host texel ceiling makes it worse: it is the ONLY memory bound in the system and it
bounds an allocation. A host with the disk for a 16K map but not the RAM can only lower the
ceiling, which refuses the map it wanted.

## What Changes

- **A regioned bake path.** A map can be baked in full-width horizontal regions whose rows
  are handed to a consumer (a callback, or a streaming PNG/EXR writer) in ascending order,
  each row exactly once. The whole output is never held in memory: the texels in flight are
  one region plus an overlap (a "halo") above and below it.
- **Two bounds, stated apart.** The host texel ceiling keeps its check and is restated as a
  bound on the OUTPUT. A new per-request WORKING-SET bound governs the in-flight region. A
  16K request under a small working-set bound is baked in more, smaller regions and is
  never refused for it; a bound too small for even one row plus its halo is honoured as far
  as it can be and the working set actually held is REPORTED.
- **Region boundaries are invisible.** The assembled output is texel-for-texel identical to
  the unregioned bake. Shading is per texel and every texel keeps its GLOBAL coordinates, so
  nothing a texel reads changes with the region it lands in; every stage that reads a
  texel's NEIGHBOURHOOD runs over a window overlapped by a halo of
  `max(2 * paddingRadius, derivative footprint)` rows. The padded band is the stage that
  bites today (#90), and its halo is TWICE the radius, not the radius: the extrapolating
  stencil reads two texels out per ring.
- **Global quantities stay global.** Every quantity normalized over the whole image is
  computed over the whole image, never per region: the relative UV-density mean (over the
  whole UDIM set), the padded band's compounding clamp range (per output image), and the
  field-sampled curvature auto-range (per output image). The object-space bounds, the
  mesh curvature auto-range and the id table are already whole-mesh quantities. A regioned
  bake gets the image-wide ones by SHADING ONCE into a scratch file on disk, measuring
  them there, and then assembling from that file — never by shading twice.
- **Streaming image writers.** PNG and EXR can be written one band at a time, byte-identical
  to the one-shot writers, so a 16K map reaches disk without existing in memory.
- **Cancellation inside a region; smooth progress.** Cancellation is polled at a fixed
  stride inside a region's rasterization, shading, scratch I/O and padding, so its latency
  is bounded by work proportional to the working set, never to the output. Progress is
  reported per texel inside a region (and now for the rasterized maps too), so it does not
  step once per region.
- **Every entry point.** `CyberBakeParams` and `CyberBundleParams` gain an appended
  `maxWorkingSetTexels` (C ABI 2.1, additive under the sized-struct rule); a new
  `cyber_bake_regions` streams one map's rows to a host callback; `cyber_image_regions`
  reports the region facts; the export bundle (and so the CLI, via `--bake-working-set`)
  streams its maps to disk when a bound is set; Python and Swift expose the same surface.
- **The declared resolution rises to 16384²** in `surface-baking`.

## Capabilities

### New Capabilities

_None._

### Modified Capabilities

- `surface-baking`: raises the declared output resolution to 16384², restates the texel
  ceiling as a bound on the OUTPUT, and adds regioned baking with a working-set bound, the
  halo rule, the global-quantity rule and a stated cancellation latency.
- `mesh-io`: a baked map SHALL be writable one horizontal band at a time, byte-identical to
  a one-shot write, and an export bundle SHALL accept a working-set bound.
- `engine-bindings`: the C ABI, Python and Swift expose the working-set bound and the
  regioned bake.

## Impact

- `src/bake/`: `bake.hpp` (working-set bound, region plan, sink, `bakeRegions`), `bake.cpp`
  (row-ranged rasterization, the region loop), `border_padding.*` (an externally measured
  clamp range, a counted row range), a new `region_scratch.*`.
- `src/imageio/`: `stream.hpp` / `stream.cpp` (band writers).
- `src/exportbundle/`: `BundleParams::maxWorkingSetTexels`, the streaming map sink,
  region facts in `BundleFile`.
- `capi/`: ABI 2.1 — appended members, `cyber_bake_regions`, `cyber_image_regions`,
  `cyber_bundle_result_file_regions`; manifest `capi/abi/cyber_capi-2.1.json`.
- `apps/cli/`: `--bake-working-set`, region facts in the JSON report.
- `python/`, `swift/`: the same surface.
- Docs: `CHANGELOG.md`, `README.md`, `docs/` where the 4096 figure or the ceiling's meaning
  is quoted.
