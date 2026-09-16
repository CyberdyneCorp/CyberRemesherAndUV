## Design

`ResourceLimits` is an optional C++ pipeline argument, so existing callers
retain byte-identical behaviour with all zero limits. `maxInput*` is checked
before `Mesh work = input`; `maxIntermediate*` is checked before an isotropic
edge split and after every island's quadrangulation; `maxOutput*` is checked
before committing the final output.

The C ABI cannot grow `CyberRemeshParams` without breaking its layout, so a
new `CyberRemeshLimits` struct is used only by new `*_with_limits` functions.
The default initializer writes disabled ceilings. Failure reports the stage,
requested count and allowed count, returns `CYBER_ERR_RUNTIME`, and does not
modify the caller input.

No field is named an RSS cap. A topology ceiling has an exact, testable
meaning; estimated RSS would not be a hard allocation guarantee.

The native QuadCover direct path additionally receives two explicit,
allocation-scoped ceilings: one for the persistent sparse Cholesky factor and
one for candidate meshes retained by `quality=best`. The sparse ceiling is
checked before allocating the factor's owned column/index/value buffers after
symbolic fill is known. The candidate ceiling is checked before copying an
input candidate and charges the exact owned mesh storage buffers that can be
retained concurrently. A failure is reported as a resource limit and leaves
the caller mesh unchanged. These ceilings do not cover transient STL or
dependency allocations and SHALL document that boundary rather than imply a
general heap cap.
