# Design: quadcover-default-and-auto-uv-atlas

## Seamless-UV backends (why three)

`quad-cover` resolves to a seamless-UV solve behind `computeSeamlessUv`, tried in
order:

1. **Vendored Geogram (in-process)** — built only with `-DCYBER_WITH_QUADCOVER`
   (the `cpu-headless` shipping preset). Reference quality; the reason the default
   beats QuadriFlow on organic meshes. MIT, no GPL.
2. **Native (always compiled)** — a dependency-free standalone seamless solver so
   the method works with no Geogram and no external binary. A few degrees lower
   median than the vendored field on organic meshes, and it is **feature-aware**
   (hard feature seams + patch pinning), which matters for CAD.
3. **External CLI** — an optional reference path via `CYBER_QUADCOVER_CLI`.

The earlier "requires the CLI, fails cleanly without it" contract was wrong once
the native solver landed (M4c); this change makes the specced default reflect
that the method always works.

## Feature routing (CAD)

The vendored Geogram extractor trails QuadriFlow on CAD parts mostly by *global
squareness* (its quads are ~4° less square even far from creases), while the
native feature-aware solver makes squarer CAD quads. So crease-heavy meshes route
to the native solver first. The discriminator is `creaseEdgeFraction` — the
fraction of interior two-face edges sharper than 45° — which cleanly separates
CAD parts (fandisk ~4%) from smooth organics (< 0.2%). Verified count-matched:
fandisk median 80.7 → 83.4 at 0 defects, organics byte-identical. Both a
threshold (`CYBER_QC_ROUTE_CREASE`) and a kill-switch (`CYBER_QC_NO_ROUTE`) exist.

## Automatic UV atlas pipeline

Mesh in, packed atlas out, no hand-drawn seams:

`autoSeams` (normal-coherent chart growth) → **cone merge** (union within one
normal cone: fewer seams, no distortion rise) → **distortion-bounded merge**
(LSCM the candidate union, accept if combined conformal + area-spread distortion
≤ cap: folds developable regions together, closing the chart-count gap to xatlas)
→ per-chart **LSCM** (planar-projection fallback for degenerate charts) →
**min-area re-orientation** (rotate each chart to its minimum-area bounding box)
→ **skyline pack** into the unit square. It reports chart count, max/RMS conformal
error, flip/fallback counts, packed area and texel density.

Each stage is independently toggleable (for A/B and testing) via `AtlasOptions`
and the mirrored C-ABI / Python parameters.

## Deferred

True 2D (overhang / hole-filling) UV nesting is the remaining coverage gap to
xatlas. A raster/profile nester was implemented and measured; overlap-safe it
regressed coverage, so it was reverted. Closing the gap needs full occupancy-grid
nesting with overhang collision + rotation search — perf-sensitive, out of scope
here.
