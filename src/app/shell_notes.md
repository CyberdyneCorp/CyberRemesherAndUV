# Application shell notes (task 8)

This module (`cyber_app`) provides the **platform-independent** core of the
application shell. GUI/windowing is intentionally out of scope here; the native
desktop shell lives in `apps/desktop` (owned elsewhere) and links this library.

## What is real and verified (tasks 8.1-8.3)

- **`document.hpp/.cpp` — `Document` (8.1).** Holds the Target mesh, EditMesh,
  canonical remesh `Parameters`, and `BakeState`. Serializes to a versioned byte
  container: `magic ("CYDC") + format version + section count`, then
  length-prefixed sections (`Target`, `EditMesh`, `Parameters`, `BakeState`).
  Unknown sections are skipped by their length, so a newer file loads in an
  older build without corruption. `load` is fully bounds-checked and returns
  `std::nullopt` on truncation/mismatch. A dirty flag drives
  `autosaveIfDirty(sink)`, which serializes and clears the flag only when dirty.

- **`undo.hpp/.cpp` — `UndoStack` (8.2).** Command journal: each `Command` has
  `apply`/`revert` and an `estimatedBytes()` cost. `push` applies the command,
  clears the redo branch, and enforces a byte budget by evicting the *oldest*
  undoable commands (always keeping the most recent action undoable). A
  `JournalMetadata` snapshot (labels + costs + cursor) serializes through the
  same byte primitives so it can be persisted **alongside** the document
  autosave for crash-recovery UI. Replaying concrete command payloads across a
  restart is left to concrete command types.

- **`input.hpp/.cpp` (8.3).** Pure, testable input logic: `StrokeCapture`
  (position + pressure + time samples), `ModifierState` (chorded modifiers),
  `DoubleTapRecognizer`, `PressHoldRecognizer`, and `HoverState`. All driven by
  synthetic timed events — no platform dependency.

## What is best-effort (tasks 8.4-8.6)

- **`shell_desktop.hpp`** describes the desktop wiring surface: `IShellWindow`,
  `IPanel`, `ShellHost`, and a **real** `ShortcutMap` (chord + key -> command
  name). It pulls in no GUI toolkit, so it compiles on headless CI, but the
  concrete window/menu/event-loop backing needs a real desktop toolkit and a
  windowing session and cannot be exercised in this environment. Marked
  `UNVERIFIED:` at the top of the header.

### Desktop shell wiring sketch (for `apps/desktop`)

1. Construct a `ShellHost` referencing the live `Document` and `UndoStack`.
2. Feed platform pointer events into `ShellHost::stroke` / recognizers and
   keyboard events into `ShellHost::modifiers` + `ShortcutMap::resolve`.
3. Route resolved command names to `UndoStack::push(...)` of concrete commands.
4. Register panels (viewport, parameters, bake) via `IShellWindow::addPanel`.
5. Run `Document::autosaveIfDirty` on a timer, writing the document buffer and
   `UndoStack::serializeMetadata` side by side.

## Build

Guarded behind `CYBER_BUILD_APP` (default OFF). Tests in `tests/app` cover the
document round-trip, budgeted undo/redo eviction, and the gesture recognizers.
