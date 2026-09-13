## MODIFIED Requirements

### Requirement: Swift package is the supported path to the library on iPad
The project SHALL ship a Swift package (SwiftPM; iPadOS/iOS and macOS) wrapping the same C ABI with idiomatic Swift — typed `throws` errors, value-type parameters, async/await for long operations with progress, Task-cancellation bridging — sufficient to build a complete iPad experience on top of it: document/session control, all tool actions, forwarding of UIKit/PencilKit touch and stylus events into the input layer, viewport attachment to a caller-supplied `CAMetalLayer`, and export/bake. The project's own iPadOS shell SHALL consume this package (not private hooks), guaranteeing third parties get the same capability surface.

For iOS consumers, the supported package distribution SHALL resolve its native C ABI from a versioned XCFramework rather than requiring repository-relative unsafe include or linker flags. It SHALL support both device and simulator builds under the same public Swift API and SHALL document the CPU solver profile and optional-solver availability of the distributed artifact.

#### Scenario: Third-party iPad app hosts the library
- **WHEN** an external iPad app adds the Swift package, attaches a Metal layer, loads a Target, and forwards Apple Pencil events
- **THEN** stroke-based retopology SHALL function inside that app with the same behavior as the first-party shell

#### Scenario: Swift task cancellation
- **WHEN** a remesh launched via the Swift async API has its enclosing Task cancelled
- **THEN** the engine SHALL cancel cooperatively and the call SHALL throw the cancellation error

#### Scenario: Packaged iOS host builds without source paths
- **WHEN** a third-party iOS app adds the binary-backed package from a clean checkout
- **THEN** it SHALL compile the public Swift API without configuring a path to `capi/include` or a locally built native library
