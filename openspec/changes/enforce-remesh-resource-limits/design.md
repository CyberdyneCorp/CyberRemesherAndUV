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

No field is named a memory or RSS cap. A topology ceiling has an exact,
testable meaning; estimated RSS would not be a hard allocation guarantee.
