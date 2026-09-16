## MODIFIED Requirements

### Requirement: Long operations are cancellable and atomic
Any long-running operation launched from the UI (remesh, unwrap, pack, bake) SHALL show determinate progress, offer a cancel control, keep the rest of the UI responsive, and commit results atomically — a superseded or cancelled run SHALL never flash stale results into the viewport or document.

The Cancel control SHALL request cancellation from the underlying operation itself, not merely stop a progress observer. The shell SHALL keep the last committed document visible until a successful operation has produced a complete replacement, and after cancellation or failure SHALL clear its busy state and retain the previously committed document.

#### Scenario: Parameter change during a run
- **WHEN** the user changes a parameter while a remesh is running
- **THEN** the running remesh SHALL be cancelled (or its result discarded) and a new run started; the stale result SHALL never be displayed (AutoRemesher flashed it)

#### Scenario: Cancelling from the iPad overlay
- **WHEN** a user selects Cancel while a remesh is running from the iPadOS progress overlay
- **THEN** the remesh job SHALL receive a cancellation request and the displayed document SHALL remain unchanged
