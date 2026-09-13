# Enforce per-operation remeshing resource limits

## Why

Import ceilings alone cannot protect an interactive host from automatic
retopology: the pipeline copies, triangulates and adaptively refines an input
before quadrangulation. A legitimate small input can therefore expand past a
phone application's working topology budget.

## What changes

- Add an opt-in `ResourceLimits` value to the C++ pipeline with independent
  input, intermediate and output vertex/face ceilings, plus scoped ceilings
  for the native direct sparse factor and retained candidate meshes.
- Check input limits before the pipeline's first full-mesh copy; thread the
  intermediate topology ceiling into the isotropic split pass; validate each
  island result and the final pure-quad subdivision before it becomes a
  returned mesh.
- Add ABI-additive C entry points accepting a `CyberRemeshLimits` POD and
  expose the plain remesh path through Python and Swift.

## Non-goals

This change does not claim a process RSS limit or a general allocator hook:
third-party sparse solvers and STL containers do not expose a single portable
allocation-accounting interface. The limits here are hard topology ceilings
and named, exact owned-storage ceilings; dependency allocation limits remain
separately documented and audited.
