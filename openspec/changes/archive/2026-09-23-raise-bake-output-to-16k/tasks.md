## 1. Bake core: row ranges

- [x] 1.1 `Texel` gains an image-local `row`; `rasterize()` takes a half-open output row range, rejects a sub-triangle outside it before any per-texel work, keeps `py` GLOBAL, and polls cancellation every 1024 faces
- [x] 1.2 `shadeTile` takes the row range and allocates a region-sized image; every image write and `PadCoord` uses `row`; `bake()`/`bakeUdim()` pass the whole range and are unchanged bit for bit
- [x] 1.3 The rasterized and field shading paths report per-texel progress (`TexelProgress`) like the ray-traced path
- [x] 1.4 The field path can defer an auto-ranged curvature encoding (raw samples out, encode later)

## 2. Padding: global range, counted rows

- [x] 2.1 The compounding clamp range is a value (`PadRange`) measured by one accumulator, used by the ordinary path and injectable into a window
- [x] 2.2 `padBorders` overload taking an external range and a counted row range for `texelsFilled`

## 3. Regioned path

- [x] 3.1 `BakeParams::maxWorkingSetTexels`, `kDerivativeFootprintTexels`, `regionHaloRows()`, `planRegions()`
- [x] 3.2 `RegionSink`, `RegionRows`, `RegionedTile`, `RegionedBakeResult`, `bakeRegions()` in `bake.hpp`
- [x] 3.3 Scratch file store (`region_scratch.*`), removed on every exit
- [x] 3.4 Shade pass: per region, rasterize + shade + spill; density sum and deferred field samples accumulated
- [x] 3.5 Finalize pass: deferred normalizations applied, padded-band range measured over final covered values
- [x] 3.6 Assembly pass: windows of `rows + 2*halo`, padded against the global range, own rows to the sink
- [x] 3.7 One-region sets take the in-memory path; refusals, ceilings and field-contract failures match `bakeUdim`
- [x] 3.8 Progress split `[0,0.9]` shade by rows, `[0.9,1]` assembly; cancellation polled at the stated units

## 4. Streaming writers

- [x] 4.1 `imageio::ImageStreamWriter` (PNG, EXR) with `open`/`writeRows`/`finish`; 1-channel PNG expands to RGB
- [x] 4.2 Byte-identity with the one-shot writers, which are left untouched

## 5. Entry points

- [x] 5.1 Bundle: `BundleParams::maxWorkingSetTexels`, streaming map sink applying green flip and sRGB per band, scratch beside the output, region facts in `BundleFile`
- [x] 5.2 CLI: `--bake-working-set`, region facts in the JSON report
- [x] 5.3 C ABI 2.1: appended members with engine defaults (floors unchanged), `cyber_bake_regions`, `cyber_image_regions`, `cyber_bundle_result_file_regions`; manifest regenerated and repinned
- [x] 5.4 Python: `max_working_set_texels`, `bake_regions`, region facts
- [x] 5.5 Swift: the same surface for bakes (Swift binds no bundle writer, so there is no bundle bound to add)

## 6. Tests

- [x] 6.1 Regioned == unregioned for every map type at several region heights (ordinary and UDIM)
- [x] 6.2 Gradient crossing a region boundary: no discontinuity
- [x] 6.3 Padded band crossing a region boundary equals the unregioned band (and a halo of `radius` would not)
- [x] 6.4 Relative density in regions divides by the whole map's (whole set's) mean
- [x] 6.5 Working set pinned: peak in-flight texels within the bound; rows ascending, once each
- [x] 6.6 8192 and 16384 outputs for every map type under a small bound; a sub-floor bound reported, not refused
- [x] 6.7 Cancellation stops inside the first region; progress has several steps per region
- [x] 6.8 Streaming writers byte-identical to one-shot at several band sizes, 1/3/4 channels
- [x] 6.9 Bundle with a bound writes byte-identical files; CLI flag and report
- [x] 6.10 C ABI: 2.0-sized caller gets the default, nothing written past its size; regioned C bake equals `cyber_bake`
- [x] 6.11 Python and Swift regioned bakes
- [x] 6.12 Registered in `tests/CMakeLists.txt`

## 7. Docs

- [x] 7.1 `CHANGELOG.md` under `## [Unreleased]`
- [x] 7.2 `README.md` / docs that quote the 4096 figure or the ceiling's meaning
