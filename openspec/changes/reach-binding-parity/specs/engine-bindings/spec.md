## MODIFIED Requirements

### Requirement: Binding parity and release discipline
Python and Swift bindings SHALL be version-locked to the engine release and covered in CI on every supported platform lane (Python: desktop OSes; Swift: macOS + iOS simulator). New ABI entry points SHALL fail CI until both bindings expose them or a pending registration exists.

The gate SHALL be a runnable check in the test suite, not a convention: `tests/packaging/test_swift_abi_parity.py` (ctest case `swift_abi_parity`) fails when a Swift source references a C symbol the header does not declare or declares with a different arity, and per-surface parity checks in the Python test suite fail when a header symbol is added without being bound. A capability the ABI exposes but the bindings do not SHALL be recorded as a pending registration, so the gap is visible rather than assumed absent.

The gate SHALL run in BOTH directions. Checking only that a binding references nothing absent from the header cannot detect an entry point no binding reaches, which is the drift that occurred: a mobile host held the complete stroke grammar and none of the operations that apply a recognized gesture. Every declared `cyber_*` entry point SHALL therefore be bound in each binding or listed in that binding's checked-in pending-registration list, and an unlisted, unbound entry point SHALL fail CI.

The header -> binding direction SHALL be one shared check applied to every binding, not a per-binding copy, so the checks cannot drift apart the way the bindings did. It SHALL run without a built engine, so it covers the lanes where the library does not load. A symbol named only in a comment or string literal SHALL NOT count as bound.

A pending registration SHALL be rejected when the header no longer declares the entry point, and SHALL be rejected when the binding already binds it, so the list can be read as an accurate statement of what is missing without being re-checked.

A host embedding the library on a mobile platform SHALL be able to reach the retopology surface end to end through Swift: Target snapping, face and strip construction, contours, boundary fill, surface cut, patch clone, loop operations and guided remeshing — and SHALL be able to finish an asset: UV unwrapping and baking. The desktop Python binding SHALL reach the same drawing surface, including stroke interpretation, so the gesture path has regression coverage on the harness that can run it.

#### Scenario: Surface drift is caught
- **WHEN** an ABI entry point is added without a matching Python or Swift wrapper
- **THEN** the parity check SHALL fail naming the missing wrapper

#### Scenario: The gate is executable
- **WHEN** the test suite runs on any platform, including those without a Swift toolchain
- **THEN** the source-level parity gate SHALL run as an ordinary test case and fail on a symbol mismatch

#### Scenario: An unbound entry point is not silently tolerated
- **WHEN** a declared `cyber_*` entry point is bound by neither the Swift sources nor that binding's pending-registration list
- **THEN** the parity gate SHALL fail naming that entry point

#### Scenario: A mobile host can apply a recognized gesture
- **WHEN** a Swift host interprets a stroke as a build action and applies it
- **THEN** the corresponding retopology entry point and a Target snapper SHALL both be reachable from Swift

#### Scenario: Python has the same gate
- **WHEN** a declared entry point is bound by no Python source and not registered as pending
- **THEN** the Python parity gate SHALL fail naming it, without needing the engine library to load

#### Scenario: A registration for a bound entry point is rejected
- **WHEN** a binding's pending-registration list names an entry point that binding already binds
- **THEN** the gate SHALL fail naming it, so the list stays an accurate statement of what is missing

#### Scenario: One binding runs the whole workflow
- **WHEN** a Swift host snaps to a Target, builds a contour tube, unwraps it and bakes a normal map
- **THEN** every step SHALL be reachable from Swift without calling the C ABI directly
