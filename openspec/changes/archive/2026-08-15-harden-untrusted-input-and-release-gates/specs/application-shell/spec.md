# application-shell (delta)

## MODIFIED Requirements

### Requirement: Autosave and document lifecycle
The application SHALL autosave the document (mesh, UVs, cage, parameters, history) at bounded intervals and on backgrounding; SHALL support explicit save, save-as-new-version, and revert-to-version; SHALL mark unsaved changes visibly; and a successful save SHALL clear the unsaved indicator. Documents SHALL use a versioned container format that does not preclude future multi-object scenes.

The container SHALL persist, for both the Target and the EditMesh, the mesh attribute columns (per-corner UVs and normals, vertex colours, and any other column a mesh carries) and the feature-edge tags, not merely positions and face indices — an autosave that loses UV work is a data-loss defect, not a limitation. Because serialization writes each mesh compacted (dead elements dropped, survivors renumbered), every id-keyed annotation SHALL be written in that same compacted order and restored onto the elements rebuilt from it; an annotation naming an element the reloaded mesh does not have SHALL be dropped rather than re-keyed onto a different element. Adding a section SHALL NOT move the format version: readers skip an unknown section by its length prefix, and a version bump would make older binaries refuse files they can in fact read.

#### Scenario: Save clears dirty flag
- **WHEN** a document with unsaved changes is saved successfully
- **THEN** the unsaved indicator SHALL disappear (AutoRemesher's `*` persisted forever)

#### Scenario: A round trip returns the document that was saved
- **WHEN** a document whose meshes carry per-corner UVs, vertex colours and tagged feature edges is saved and loaded again
- **THEN** those attributes and tags SHALL be present on the reloaded meshes, and a comparison that inspects only topology SHALL NOT be treated as evidence that the round trip is lossless

#### Scenario: Named selection slots survive a hole in the id space
- **WHEN** a document is saved after edits have left dead vertices, and is then loaded
- **THEN** each named soft-selection slot SHALL address the same vertices it addressed before the save, and weights naming vertices the reloaded mesh no longer has SHALL be absent rather than applied to their successors

#### Scenario: An added section does not lock out older readers
- **WHEN** a document written by a build that emits the attribute sections is opened by a build that predates them
- **THEN** the older build SHALL load the document, skipping the sections it does not know by their length prefix

#### Scenario: A save that never reaches the device is reported
- **WHEN** a document save fails while its buffer is being flushed (full disk, over-quota mount)
- **THEN** the call SHALL report failure rather than success, and the unsaved indicator SHALL remain

### Requirement: Diagnostics
The application SHALL provide an in-app log view with save-to-file, gate verbose/debug logging behind an explicit developer setting (silent by default — AutoRemesher spewed per-element debug lines unconditionally), and include build/version/GPU-backend info in an About panel.

The engine SHALL also be a well-behaved guest in a host process it does not own: no engine call, and no third-party code an engine call initializes, SHALL replace the host's signal handlers, `std::terminate` handler or `std::new_handler`, alter process-global locale settings, or repoint the host's standard stream buffers. Quieting the engine's own output SHALL never quiet the host's. Any process-global state an engine call must touch SHALL be restored, and anything deliberately left changed SHALL be documented at the call site with its reason.

#### Scenario: Quiet by default
- **WHEN** a large mesh is remeshed with default settings
- **THEN** no per-element debug output SHALL be produced

#### Scenario: A host's crash reporter survives a remesh
- **WHEN** a host process that has installed its own signal, terminate and new handlers, set its locale, and attached its own stream buffers runs a remesh on the default quad method
- **THEN** all of those SHALL still be in place afterwards, and log lines the host writes while the solve runs SHALL appear where the host directed them

#### Scenario: A long-lived host does not grow per call
- **WHEN** the same process runs many remeshes in sequence
- **THEN** no per-call state SHALL be retained in process-global registries owned by the engine or its vendored dependencies, and reachable memory SHALL NOT grow with the number of calls
