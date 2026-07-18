# Shared mobile UI model (task 8.6)

> **UNVERIFIED / best-effort.** Not built or validated in CI. This directory is
> the single source of truth for the cross-platform UI *model* — the parts of
> the shell that must behave identically on iPadOS and Android. Data lives in
> the JSON files here; behavioural contracts are specified below and
> implemented natively per platform (SwiftUI / Jetpack Compose) against them.

The engine (C++ `src/app`, behind `capi/cyber_capi.h`) owns all
*behaviour that touches geometry*: stroke recognition, chording, tool routing,
undo. The shells own *presentation*. Keeping the presentation model here — as
data plus written contracts — is what makes iPad and Android stay in lockstep:
add a tool to `toolbar.default.json` and it appears on both, wired to the same
engine `command`.

## Files

| File | Role |
|------|------|
| `stages.json` | Stage switcher model: Retopology → UV → Baking |
| `toolbar.default.json` | Action Gallery (all actions) + default configurable toolbar |
| `tutorial.json` | Tutorial content outline |
| `README.md` | This document — the behavioural contracts |

Both shells decode these files: iPadOS via `Codable`
(`ipados/.../UIModel.swift`), Android via kotlinx-serialization
(`android/.../ui/UiModel.kt`).

## 1. Stage switcher

A single-select segmented control listing the stages from `stages.json` in
order. Switching stages:

- never destroys the document; it changes which tools/overlays are active;
- filters the toolbar to actions whose `stages` array contains the stage id (an
  empty `stages` array = available in every stage, e.g. **Move**);
- is a pure UI transition — the engine session is stage-agnostic.

## 2. Action Gallery + configurable toolbar

- **Action Gallery** = every entry in `toolbar.default.json → actions`. It is
  the full palette a user can pull from.
- **Toolbar** = an ordered subset (`defaultToolbar`) the user can reconfigure
  (add/remove/reorder). The default mirrors the behavioural reference:
  Pencil, Relax, Move, Tweak, Erase.
- Each action carries an opaque engine `command` (e.g. `tool.pencil`). The shell
  **never interprets** it — selecting an action injects that command id into the
  session (`inject(chord:)` / the Android JNI equivalent) and the engine decides
  what it means. This is what keeps tool semantics coherent across stages.
- A user's toolbar customization is shell-local preference state (UserDefaults /
  SharedPreferences); it is not engine state.

## 3. Long-operation progress / cancel with **atomic commit** (no stale flash)

The critical contract. A long op (remesh, bake, flatten) runs with a progress +
cancel overlay. The rules both shells MUST honor:

1. **The viewport keeps showing the committed document for the entire run.** The
   in-progress result is never partially drawn.
2. **The result is swapped in exactly one step, only on success.** Build the new
   session/render state first, then publish the new mesh + dismiss the overlay
   in a single UI transaction. There is no intermediate frame where the old
   geometry is gone but the new geometry is not yet ready — that "stale flash"
   is the specific defect this contract forbids.
3. **Cancel restores nothing because nothing changed.** Cancellation is
   cooperative (the engine polls the cancel callback < 100 ms) and unwinds
   leaving inputs untouched; the shell just dismisses the overlay. The document
   is byte-for-byte what it was before the op started.
4. **Progress is monotonic-ish and advisory.** Never gate the commit on progress
   reaching exactly 1.0; gate it on the operation's success result.

Reference implementations:
`ipados/.../AppModel.swift` (`LongOperationState` + `commit`/`rollback`) and
`android/.../EngineSession.kt` (`OperationState` + `commit`/`rollback`).

## 4. Tutorial content

`tutorial.json` is a linear, chaptered outline (Welcome → Retopology → UV →
Baking). Content only — no engine coupling — so it can be localized and updated
without a shell rebuild. Each step is a short imperative line suitable for a
coach-mark or a first-run walkthrough.

## 5. In-app log view (quiet by default)

- A bounded ring buffer of leveled entries (`DEBUG/INFO/WARN/ERROR`).
- **Quiet by default:** only `INFO` and above are shown; the user can flip a
  "Verbose" toggle to include `DEBUG` for diagnostics/bug reports.
- The log is shell-side observability (engine calls, autosave, op lifecycle). It
  is not the engine's own logging; engine errors surface here via the typed
  error strings from `cyber_last_error`.
- Reference: `ipados/.../Log.swift` + `LogView.swift`,
  `android/.../ui/LogModel.kt`.

## Schema versioning

Every JSON file carries `schemaVersion` (currently `1`). Shells must reject a
higher major schema they don't understand rather than silently dropping fields,
and fall back to a built-in minimal model if a file is missing.
