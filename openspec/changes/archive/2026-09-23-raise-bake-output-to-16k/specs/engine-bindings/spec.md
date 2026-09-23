## ADDED Requirements

### Requirement: Regioned baking is reachable from the bindings

The C ABI SHALL expose the working-set bound as a member APPENDED to the sized
`CyberBakeParams` and `CyberBundleParams`, which is additive under "Sized parameter structs"
(ABI 2.1); a caller stating the 2.0 size SHALL get the default of zero (no bound), and no
byte past its stated size SHALL be read or written. The C ABI SHALL expose a regioned bake
of one map that hands each band's rows to a host callback in ascending order, accepts an
optional field evaluator, reports progress and honours cooperative cancellation, and whose
result carries every encoding, padding and id record an ordinary bake's image carries. A
host callback that asks to stop SHALL abandon the bake with an I/O status. The region facts
(region height, halo, region count, working set held) SHALL be readable for a baked image
and for every map of a bundle. Every parameter an ordinary bake validates SHALL be validated
identically by the regioned bake, and the host's texel ceiling SHALL apply to its output.
The entry points that return a whole image accept the working-set bound and do not read it,
because their result is the whole output by construction.

Python and Swift SHALL expose the same regioned bake and the working-set bound on bakes,
setting `structSize` themselves. Python SHALL also expose the bound on bundles; Swift binds no
bundle writer at all, so it has none to extend. A binding whose integer type can hold a
negative bound SHALL refuse one rather than let it wrap to the unsigned "no bound" range.

The image a regioned bake returns carries the map's metadata and NO pixels. Every entry point
that encodes an image's pixels SHALL refuse such an image with `CYBER_ERR_INVALID_ARG` and
never read past its empty buffer.

#### Scenario: A 2.0 caller gets the default working-set bound
- **WHEN** a caller states the ABI 2.0 size of `CyberBakeParams` or `CyberBundleParams`
- **THEN** the defaults call SHALL write nothing past that size and the bake SHALL run with no working-set bound

#### Scenario: A regioned bake through the C ABI equals the ordinary bake
- **WHEN** a host bakes a map through the regioned entry point with a bound that splits it into several regions
- **THEN** the rows its callback receives, assembled, SHALL equal the ordinary bake's pixels, and the region facts SHALL report more than one region

#### Scenario: The bindings stream a regioned bake
- **WHEN** the Python or Swift binding runs a regioned bake
- **THEN** it SHALL deliver the rows in ascending order and the assembled map SHALL equal the ordinary bake

#### Scenario: Saving a regioned bake's image is refused
- **WHEN** a host saves the image a regioned bake returned, for a one-, three- or four-channel map
- **THEN** the save SHALL fail with `CYBER_ERR_INVALID_ARG`, SHALL write no file, and the process SHALL NOT crash

#### Scenario: A negative bound is refused by the binding
- **WHEN** the Python binding is given a negative working-set bound for a bake or a bundle
- **THEN** it SHALL raise before calling the engine rather than bake with no bound
