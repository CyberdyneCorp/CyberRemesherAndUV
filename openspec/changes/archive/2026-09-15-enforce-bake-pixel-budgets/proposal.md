# Enforce host bake-pixel budgets

## Why

A bake allocates both UV-rasterized texel work and its final float image from
`width * height`. A mobile host previously had no way to reject an accidental
16k texture before those allocations began.

## What changes

- Add an opt-in `BakeParams::maxPixels` guard for direct C++ callers.
- Add process-level C ABI and Python controls, preserving the ABI by additive
  functions only.
- Check the requested texel product before rasterization, overflow-safely, and
  return a diagnostic naming the request and configured ceiling from the C ABI.

## Non-goals

This is an exact texel allocation ceiling, not a process RSS estimator and not
a substitute for the high-poly BVH or renderer memory budgets.
