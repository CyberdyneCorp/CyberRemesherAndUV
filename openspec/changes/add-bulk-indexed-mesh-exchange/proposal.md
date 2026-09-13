## Why

The Swift SDK creates indexed geometry through one C call per triangle and the
public C ABI only exports render triangulation. Hosts that already own polygon
buffers therefore cannot exchange authored quads and n-gons efficiently or
without losing topology.

## What Changes

- Add an additive, copying C ABI descriptor for packed XYZ positions and CSR
  face offsets/vertex indices.
- Add matching authored-polygon export with a query-then-copy ownership
  contract.
- Expose the same geometry and typed attribute import/export surface in Swift
  and Python, and retain existing triangle
  helpers unchanged.

## Impact

The C ABI receives additive structs/functions and a minor ABI version bump;
Swift and Python receive polygon-aware mesh initializers/export. Rust receives
the documented geometry-only subset.
