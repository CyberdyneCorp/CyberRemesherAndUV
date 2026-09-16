## Design

The bake core checks positive width and height with division rather than a
potentially overflowing product. If `maxPixels` is non-zero, it rejects when
`width * height` would exceed it before calling `rasterize()` or `makeImage()`.

The C ABI retains its layout-frozen `CyberBakeParams`; an additive pair of
global setter/getter functions snapshots a default budget into C++ bake params
at the entry point. This mirrors the existing import resource controls and
gives C and Python hosts a usable error message without changing old callers.
