# application-shell — Desktop and Mobile Application Layer

## ADDED Requirements

### Requirement: Stage structure
The application SHALL organize work into four stages — **Remesh** (automatic), **Retopo**, **UV**, **Bake** — over a single shared document (Target + EditMesh + parameters + bake state). Stage switching SHALL be instant and lossless; the core actions keep coherent semantics across stages.

#### Scenario: Remesh feeds retopo
- **WHEN** an automatic remesh completes and the user switches to the Retopo stage
- **THEN** the remesh result SHALL be editable as the EditMesh with the original input as Target

### Requirement: Input model
On touch devices: stylus strokes perform modeling gestures; fingers navigate the camera; held on-screen modifier buttons (Relax/Move/Tweak/Erase) chord with simultaneous stylus strokes; an extra finger held during Tweak/Build activates vertex snapping; stylus pressure modulates Erase; double-tap on geometry triggers Tweak; press-and-hold triggers context tools (strip draw on boundary edge, boundary auto-select on boundary vertex, loop info on interior edge); stylus hover (where supported) previews brush footprints. On desktop: equivalent mouse interactions plus keyboard shortcuts for every core action and undo/redo. All recognizers SHALL be implemented once in shared C++ and driven by platform shells.

#### Scenario: Chorded relax
- **WHEN** the user holds the Relax button with a finger and strokes with the stylus
- **THEN** the stroke SHALL relax topology instead of drawing

#### Scenario: Desktop shortcut parity
- **WHEN** a desktop user presses the documented shortcut for an action
- **THEN** the same action SHALL activate as its touch equivalent

### Requirement: Undo and redo
Undo/redo SHALL be a persistent command journal, unbounded within a configurable memory budget (default ≥ 1000 steps), covering all document mutations in every stage, with two-finger-tap undo / three-finger-tap redo on touch and standard shortcuts on desktop. The journal SHALL persist with autosave so a restart restores history.

#### Scenario: Deep undo
- **WHEN** the user performs 500 retopo operations and holds undo
- **THEN** the document SHALL step back through all 500 states (CozyBlanket caps at 30)

#### Scenario: History survives crash
- **WHEN** the application is killed and relaunched
- **THEN** the restored document SHALL include its undo history up to the last autosave point

### Requirement: Autosave and document lifecycle
The application SHALL autosave the document (mesh, UVs, cage, parameters, history) at bounded intervals and on backgrounding; SHALL support explicit save, save-as-new-version, and revert-to-version; SHALL mark unsaved changes visibly; and a successful save SHALL clear the unsaved indicator. Documents SHALL use a versioned container format that does not preclude future multi-object scenes.

#### Scenario: Save clears dirty flag
- **WHEN** a document with unsaved changes is saved successfully
- **THEN** the unsaved indicator SHALL disappear (AutoRemesher's `*` persisted forever)

### Requirement: Long operations are cancellable and atomic
Any long-running operation launched from the UI (remesh, unwrap, pack, bake) SHALL show determinate progress, offer a cancel control, keep the rest of the UI responsive, and commit results atomically — a superseded or cancelled run SHALL never flash stale results into the viewport or document.

#### Scenario: Parameter change during a run
- **WHEN** the user changes a parameter while a remesh is running
- **THEN** the running remesh SHALL be cancelled (or its result discarded) and a new run started; the stale result SHALL never be displayed (AutoRemesher flashed it)

### Requirement: Customizable tool gallery
Beyond the five core actions, advanced tools SHALL live in an Action Gallery from which the user drags tools into a configurable toolbar (≥ 14 slots, repositionable top/bottom, two button sizes); every action SHALL have an in-app help panel with a short demonstration.

#### Scenario: Add a gallery tool
- **WHEN** the user drags PatchClone from the gallery into the toolbar
- **THEN** it SHALL become immediately usable and persist across sessions

### Requirement: Onboarding
The application SHALL ship interactive tutorial content (guided exercises for strip building, loop editing, tweaking) and a bundled practice model, available offline.

#### Scenario: First launch
- **WHEN** the app starts with no document
- **THEN** the user SHALL be offered the tutorial and practice model

### Requirement: Privacy and offline operation
The application SHALL function fully offline; the only network feature SHALL be the opt-in local network bridge. No telemetry, analytics, or account system SHALL exist.

#### Scenario: Network audit
- **WHEN** the app runs with the bridge disabled
- **THEN** it SHALL open no sockets

### Requirement: Diagnostics
The application SHALL provide an in-app log view with save-to-file, gate verbose/debug logging behind an explicit developer setting (silent by default — AutoRemesher spewed per-element debug lines unconditionally), and include build/version/GPU-backend info in an About panel.

#### Scenario: Quiet by default
- **WHEN** a large mesh is remeshed with default settings
- **THEN** no per-element debug output SHALL be produced
