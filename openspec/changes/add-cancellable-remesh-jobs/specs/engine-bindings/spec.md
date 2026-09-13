## MODIFIED Requirements

### Requirement: Swift package is the supported path to the library on iPad
The project SHALL ship a Swift package (SwiftPM; iPadOS/iOS and macOS) wrapping the same C ABI with idiomatic Swift — typed `throws` errors, value-type parameters, async/await for long operations with progress, Task-cancellation bridging — sufficient to build a complete iPad experience on top of it: document/session control, all tool actions, forwarding of UIKit/PencilKit touch and stylus events into the input layer, viewport attachment to a caller-supplied `CAMetalLayer`, and export/bake. The project's own iPadOS shell SHALL consume this package (not private hooks), guaranteeing third parties get the same capability surface.

Each Swift remesh operation SHALL represent exactly one native execution. It SHALL expose idempotent explicit cancellation, borrow and retain its input until that execution has reached a terminal state, and document that callers MUST NOT mutate that input while the job is active. It SHALL give every concurrent or subsequent awaiter the same terminal result or error. Task cancellation while awaiting SHALL request cancellation of that shared job without abandoning its native worker. Progress delivery SHALL be monotonic, serialized for consumers, and finish exactly once; callbacks SHALL never outlive the operation's retained control state. A successful result SHALL be independently owned by the caller, while cancellation and failure SHALL not mutate the input.

#### Scenario: Third-party iPad app hosts the library
- **WHEN** an external iPad app adds the Swift package, attaches a Metal layer, loads a Target, and forwards Apple Pencil events
- **THEN** stroke-based retopology SHALL function inside that app with the same behavior as the first-party shell

#### Scenario: Swift task cancellation
- **WHEN** a remesh launched via the Swift async API has its enclosing Task cancelled
- **THEN** the engine SHALL cancel cooperatively and the call SHALL throw the cancellation error

#### Scenario: Explicit job cancellation and repeated awaits
- **WHEN** a caller cancels a running `RemeshOperation` and awaits its value from more than one task
- **THEN** the engine SHALL receive one cooperative cancellation request, execute at most once, and every awaiter SHALL observe the same cancelled terminal error
