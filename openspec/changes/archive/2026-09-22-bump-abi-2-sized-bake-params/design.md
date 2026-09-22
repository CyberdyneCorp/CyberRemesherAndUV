## Context

`CyberBakeParams` and `CyberBundleParams` are passed by pointer to the bake and
bundle entry points and are initialised by `cyber_default_*_params`. Both grew by
appending members under minor bumps during the 0.10.0 cycle. Their layout at the
v0.9.0 tag (ABI 1.16) is a strict prefix of today's, which is exactly why the
failure is silent: nothing crashes at link or load time, only at the first call.

The bake-provider descriptors, added in ABI 1.22, already solved this problem for
themselves with a leading `structSize`.

## Goals / Non-Goals

**Goals:** make an old binary fail loudly rather than corrupt memory; make the
next bake-parameter append additive; keep one rule for sized structs, not two.

**Non-Goals:** preserving binary compatibility for 1.x clients (that is what a
major bump gives up, deliberately); changing any bake behaviour or default;
changing the Python or Swift public API.

## Decisions

**D1. A major bump rather than frozen structs plus `_ex` entry points.** Freezing
the 0.9.0 layouts and adding size-carrying `_ex` twins would keep 1.x binaries
running, at the price of two struct families and five duplicate entry points
forever. The bindings, which are most users, each ship or build their own library
and are not exposed either way; the exposed population is C hosts that relink a
separately upgraded shared library. A loud, one-time recompile for them is cheaper
than permanent duplication for everyone. Chosen for cleanliness, with that
trade-off stated.

**D2. `structSize` first, the provider descriptors' semantics, but engine defaults
instead of zero.** The library copies `min(structSize, sizeof)` bytes of the
caller's struct over a struct already filled with the ENGINE DEFAULTS, and
`cyber_default_*_params` writes at most `structSize` bytes. The provider
descriptors default uncovered members to zero because zero is their documented
default; here zero would mean a 0x0 bake, so the base is the defaults instead. A
size larger than this build's layout (a newer caller) is served: the library reads
what it knows and ignores the rest.

**D3. The floor is frozen by `offsetof`, not `sizeof`.** The minimum accepted
`structSize` is the end of the last 2.0 member
(`offsetof(..., lastMember) + sizeof(lastMember)`). Written with `sizeof` it would
move when a member is appended and refuse the very 2.0 callers the mechanism
exists to serve — the mistake the provider result descriptor already had to
correct.

**D4. `cyber_default_*_params` return `CyberStatus`.** They depend on `structSize`
being set; a `void` initializer that silently skips an unset size hands the caller
an uninitialised struct. `NULL` or a size below the floor is
`CYBER_ERR_INVALID_ARG`, naming both numbers, and the struct is left untouched.

**D5. The placement is still validated only for maps that read it.** That rule was
introduced so a 1.22 host that zero-filled the struct did not break every bake with
a singular matrix. Under sized structs an older caller gets the identity by default,
but a 2.0 caller that zero-fills and forgets the placement would hit the same trap,
and the rule costs nothing. Kept.

## Risks / Trade-offs

- **A C host that upgrades the shared library without recompiling stops loading.**
  Intended: that is the loud failure replacing a silent one. Stated in the
  CHANGELOG under a breaking-change heading, with the two-line source migration.
- **A caller that forgets `structSize`.** Uninitialised, it is either below the
  floor (refused loudly) or at/above it, in which case the library still reads and
  writes at most `sizeof` of its own layout. Memory-safe either way.
- **The retained v0.8 client test** calls only `cyber_version`, which 2.0 does not
  change, so it stays green; it never exercised these structs, which is how the
  original break went unnoticed. It now proves only that a separate process loads
  the shared library; cross-major compatibility is no longer claimed, and the docs
  say so.
