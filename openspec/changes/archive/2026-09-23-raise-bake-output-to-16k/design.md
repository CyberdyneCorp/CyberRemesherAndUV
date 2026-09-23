## Context

Every bake allocates its whole output as one `bake::Image`. #91 split the bake into a shared
`BakeContext` (one BVH, one flattened hierarchy, one set of Target normals, one curvature
field, one object box, one id table) plus a per-tile shade/finish pair, with the density
normalization between the two so a UDIM set divides by the whole set's mean. That split is
what makes a region "one more axis of the same loop".

An earlier, never-built draft of this change (`feat/bake-tiled-16k`, 8bcd9a6) proposed
shading each region with a halo of `max(paddingRadius, derivativeFootprint)` rows and a
rasterize-only pre-pass for the density mean. Reading it against the code found two defects
that would each have produced a wrong-but-plausible seam, and they shape this design:

1. **The padding stage has a second GLOBAL quantity.** `BorderPadder::measureRange` clamps
   every extrapolated texel to `[low - span, high + span]` where `low`/`high` are the
   per-channel extremes of EVERY covered texel in the image. Measured per region, a region
   whose islands happen to span a narrower range clamps its band differently, and the seam
   comes back as a padding artefact on a bake whose shading was seamless.
2. **The padding halo is `2 * radius`, not `radius`.** The extrapolating rule reads the
   covered neighbour AND the texel beyond it (`2*v1 - v2`), so a ring-`k` texel reads two
   texels out, from a ring that itself read two texels out. Its dependency cone reaches
   `2k - 1` texels, and whether an intermediate texel is a source at all depends on coverage
   up to `2k` away. A halo of `radius` rows under-covers every band texel more than
   `radius / 2` rings out.

A third global the draft missed: with a field evaluator attached and an auto curvature
range, the field path takes the range as a percentile over the SAMPLED TEXELS of the image
— per region it would be a different number in every region.

## Goals / Non-Goals

**Goals**

- 8192² and 16384² outputs for every map type with a peak in-flight texel count the host
  declares, not one the resolution dictates.
- The assembled output identical, texel for texel, to the unregioned bake — including the
  padded band and every globally normalized quantity.
- The two bounds (output size, working set) separately settable at every entry point that
  can stream.
- The bytes reach disk without the map ever existing in memory.

**Non-Goals**

- Tiling in X. Regions are full-width horizontal bands.
- Removing the unregioned `bake()`/`bakeUdim()`. They stay the simple API and are what a
  bake with no working-set bound (the default, `0`) runs.
- Changing any map's arithmetic, or #90's padding semantics.
- Bounding the Target: the BVH, Target normals and curvature field are whole-mesh and are
  NOT counted in the working set. The bound is on the image, which is what scales with
  resolution.

## Decisions

### Shade once into a scratch file, assemble from it

A regioned bake runs in two passes over the same regions:

1. **Shade.** Each region's rows are rasterized (row-clipped), shaded into a region-sized
   image, and written with a coverage byte per texel to a SCRATCH file on disk. Nothing is
   padded or normalized yet. The per-set/per-image globals are accumulated as regions
   finish: the density sum (in raster order, exactly as `accumulateDensity` walks a whole
   image) and, for a field-sampled curvature map with an auto range, the raw samples.
2. **Finalize + assemble.** A streaming pass over the scratch applies the deferred
   normalizations (divide by the whole set's density mean; encode field curvature with the
   whole image's range) and measures the padded band's clamp range over the FINAL covered
   values. Then each region is assembled from a window of `rows + 2 * halo` scratch rows,
   padded against the whole-image clamp range, and only its own rows are handed on.

*Alternative rejected: halo-shaded regions plus pre-passes* (the draft). The density mean
has a cheap rasterize-only pre-pass, but the padding clamp range needs every covered texel's
FINAL value — a full shading pre-pass, which doubles the cost of the maps that dominate the
wall clock (AO, bent normal, thickness: a hemisphere of rays per texel). The scratch file
costs `rows * width * (4 * channels + 1)` bytes of disk — ~3.5 GB at 16384² RGB — which is
exactly the resource a host that "has the disk for it" has.

*Alternative rejected: pad after assembly.* Correct, but it needs the whole map.

*Alternative rejected: redefine the clamp range locally.* It would make regioning trivially
seamless, but it silently changes every existing map's padded band (#90's behaviour) for a
reason unrelated to those maps.

Because shading happens once per texel, a region needs NO halo for shading at all: every
texel is shaded from its own global coordinates, the Target, and whole-mesh context. This
tree's curvature and cavity maps do NOT take a screen-space derivative — they read the
Target's vertex curvature field at the cage hit — so they are texel-independent too.

### The halo, and where it applies

`halo = max(2 * paddingRadius, kDerivativeFootprintTexels)` with
`kDerivativeFootprintTexels = 1`, applied to the ASSEMBLY window, which is where every stage
that reads a texel's neighbourhood runs. Padding needs `2 * radius` (above). The derivative
footprint is declared and honoured even though no current stage takes a derivative: a future
screen-space derivative bake (ArmorPaint's) runs as a post-shade stage over the same window,
and a halo of zero would be a silent trap for whoever adds one.

### Global quantities, inventoried

| quantity | scope | how a regioned bake gets it |
| --- | --- | --- |
| relative UV-density mean | the whole image; the whole SET under UDIM | summed during the shade pass in raster order; applied in finalize |
| padded-band clamp range | one output image (per tile) | measured in finalize over final covered values; injected into every window |
| field curvature auto-range | one output image (per tile) | raw samples gathered in the shade pass; percentile after it; encoding in finalize |
| object-space bounds | both meshes | already whole-mesh, in `BakeContext` |
| mesh curvature auto-range | whole Target, area-weighted | already whole-mesh, in `BakeContext` |
| id-to-colour table | whole Target | already whole-mesh, in `BakeContext` |
| per-texel AO rotation | the texel's GLOBAL (px, py) | `Texel::py` stays global; a new `Texel::row` indexes the region image |

The field auto-range samples are the ONE per-texel buffer outside the working set: a
percentile needs every sample (4 bytes per covered texel of one image). It is stated in the
spec, and a host that sets an explicit curvature range avoids it.

### Two bounds, and only one of them can refuse

`BakeParams::maxPixels` keeps its meaning and its check: it bounds the OUTPUT (per tile, and
in aggregate for a UDIM set). `BakeParams::maxWorkingSetTexels` is new:

```
halo        = max(2 * paddingRadius, 1)
regionRows  = maxWorkingSetTexels == 0 ? height
            : clamp(maxWorkingSetTexels / width - 2 * halo, 1, height)
workingSet  = width * min(height, regionRows + 2 * halo)   (0 bound: width * height)
```

It cannot refuse. A bound below one row plus two halos yields one-row regions and reports
the working set actually held (`boundReached = false`). A bound of zero is "no policy": one
region, the in-memory path, output bit-identical to `bake()`.

### One region set, one path

A set (one image, or every occupied tile of a UDIM layout) whose regions number exactly one
runs the in-memory pipeline and emits the finished image as one band. Anything more runs the
scratch pipeline for every tile — a UDIM set whose tiles each fit in one region still spills,
because holding every tile at once is what the bound forbids.

### Streaming writers are separate from the one-shot writers, and tested equal

`imageio::ImageStreamWriter` writes PNG (one IDAT chunk whose length is known up front,
STORED deflate blocks cut on the running raw stream at 65535 bytes, running Adler-32 and
CRC) and EXR (`NO_COMPRESSION`, so the scanline offset table is computable before any
pixel). The one-shot `writePng`/`writeExr` are left untouched; a test asserts the streamed
bytes equal the one-shot bytes at several band sizes and channel counts. Two independent
encoders pinned together by a byte test are a stronger check than one encoder tested against
itself, and the one-shot writers' existing behaviour (no file created on invalid arguments)
does not move.

### Entry points

- **C++**: `bakeRegions(low, high, map, params, udim, sink, progress, cancel, scratchDir)`.
- **C ABI (2.1)**: `maxWorkingSetTexels` appended to the sized `CyberBakeParams` and
  `CyberBundleParams` (the 2.0 floors do not move; a 2.0-sized caller gets the default 0).
  `cyber_bake_regions` streams one map (tile 1001; an optional field evaluator) to a row
  callback and returns a pixel-less `CyberImage` carrying the metadata;
  `cyber_image_regions` and `cyber_bundle_result_file_regions` report region facts. The
  whole-image entry points (`cyber_bake`, `cyber_bake_udim`, `cyber_bake_field`, the
  provider) return the whole output by construction, so they accept the member and do not
  read it — documented, like a parameter a map does not read.
- **Bundle / CLI**: `BundleParams::maxWorkingSetTexels` (`--bake-working-set`) streams each
  map through the preset's green flip and sRGB encoding per band into a streaming writer.
  Scratch goes next to the output (the disk the host is writing to), not to a possibly
  RAM-backed `/tmp`.

### Cancellation and progress

Polled every 2048 shaded texels (per worker on the ray-traced path), every 1024 rasterized
faces, per scratch row, and between padding rings. Each of these is bounded by the region,
the window or the mesh, never by the output: that is the stated latency. Progress: the shade
pass takes `[0, 0.9]` of the bar, split by rows across the set's regions, with the existing
1% texel step inside each region (now also on the rasterized and field paths); finalize and
assembly take `[0.9, 1]`, reported per region.

## Risks / Trade-offs

- **Rasterization is repeated per region.** Every face is visited per region, with a cheap
  row-range reject before any per-texel work. Region counts are small at realistic bounds
  (tens); a per-row face index would be a second acceleration structure to keep correct.
- **The halo is padded twice.** At radius 8 the window is 32 rows taller than the region;
  at a floor of one row that is 33x. The report says what was held.
- **Scratch I/O.** ~3.5 GB written and read twice at 16384² RGB — seconds beside hours of
  hemisphere rays. A write failure (disk full) abandons the bake with a named failure; the
  scratch file is removed on every exit path.
- **Scratch naming.** The file is created with a random name in the scratch directory; it is
  process-private and removed on destruction.
- **Not measured at 16384² on the development host.** The test suite pins the WORKING SET
  (peak in-flight texels) against the bound while the output is far larger, and runs the
  8192²/16384² cases on a tiny chart; a full-coverage 16384² bake of every map is a manual
  measurement, not a CI test.
