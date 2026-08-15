#pragma once

#include <cstdint>
#include <functional>
#include <map>
#include <optional>
#include <span>
#include <string>
#include <vector>

#include "cyber/app/serial.hpp"
#include "cyber/core/mesh.hpp"
#include "cyber/core/remesh_params.hpp"

// Application document model (application-shell spec, task 8.1). A Document is
// the single editable unit the shell operates on: the high-poly Target, the
// low-poly EditMesh being retopologised, the canonical remesh Parameters, and
// the current bake settings/status. It (de)serialises to a versioned byte
// container (magic + version + length-prefixed sections) so unknown future
// sections are skipped rather than corrupting the load, and it carries a dirty
// flag driving the autosave hook.
//
// The section list is APPEND-ONLY: a new section takes the next unused id and is
// written only when it carries data, so a document without it is byte-identical
// to what the previous build wrote and older binaries (which skip unknown ids by
// their length prefix) keep loading new files. That is also why kFormatVersion
// does NOT move for an added section — `load` rejects version > kFormatVersion,
// so bumping it would make older binaries refuse files they can in fact read.
//
// Sections in id order: 1 Target mesh, 2 EditMesh, 3 Parameters, 4 BakeState,
// 5 soft-selection slots, 6/7 the attribute columns and feature-edge tags of
// the target and the edit mesh (both optional, see the note on save()).
namespace cyber::app {

// Persisted in the document byte container, so values are append-only: an
// existing document's stored kind must keep meaning what it meant when saved.
enum class BakeMapKind : std::uint32_t {
    Normal,
    AmbientOcclusion,
    Displacement,
    Position,
    Color,
    Curvature,
    Cavity,
};

// Persisted bake configuration and status (the pixel maps themselves are
// derived artefacts, re-baked on demand; only the settings and a revision
// marker live in the document).
struct BakeState {
    BakeMapKind map = BakeMapKind::Normal;
    int width = 512;
    int height = 512;
    float cageDistance = 0.1f;
    int samples = 16;
    bool baked = false;              // a valid map exists for the current revision
    std::uint64_t bakeRevision = 0;  // bumped on each successful bake

    friend bool operator==(const BakeState&, const BakeState&) = default;
};

class Document {
public:
    // Format identity. `load` accepts any version <= kFormatVersion.
    static constexpr std::uint32_t kMagic = 0x43594443u;  // "CYDC" little-endian
    static constexpr std::uint32_t kFormatVersion = 1u;

    Mesh target;
    Mesh editMesh;
    remesh::Parameters params;
    BakeState bake;

    // Named soft-selection slots (manual-retopology spec, "Selection
    // operations"): the raw per-vertex weight field of a
    // retopo::SoftSelection over the EditMesh, keyed by the slot name the
    // user saved it under. Ordered so serialization is deterministic. Stored
    // as plain floats to keep the app layer's dependency on the mesh kernel
    // alone. Weights are indexed by EditMesh vertex id, so a command that
    // reassigns ids (subdivide) invalidates the slots along with every other
    // id-keyed annotation.
    //
    // Serialization reassigns ids too: the mesh is written compacted (dead
    // vertices dropped, the alive ones renumbered 0..n-1), so `save` rebases
    // the slots onto that same numbering and `load` returns them keyed to the
    // dense id space `fromIndexed` rebuilds. Weights on dead ids do not
    // survive a round trip, which is the point — they name vertices the
    // reloaded mesh no longer has. The section layout is unchanged by this, so
    // kFormatVersion still does not move (see the note above).
    std::map<std::string, std::vector<float>> softSelections;

    // ---- serialization (task 8.1) -------------------------------------
    // Both meshes persist with their attribute columns (corner UVs and
    // normals, vertex colours, whatever else a column set carries) and their
    // feature-edge tags, so a reloaded document is the one that was saved
    // rather than bare positions and face indices. Columns are keyed by
    // element id, so they are written in the compacted order the mesh itself
    // is written in and restored onto the elements rebuilt from it.
    [[nodiscard]] std::vector<std::uint8_t> save() const;
    [[nodiscard]] static std::optional<Document> load(std::span<const std::uint8_t> bytes);

    // ---- dirty tracking / autosave hook -------------------------------
    void markDirty() { m_dirty = true; }
    void clearDirty() { m_dirty = false; }
    [[nodiscard]] bool dirty() const { return m_dirty; }

    // Serialises and hands the buffer to `sink` only when the document is
    // dirty, then clears the flag. Returns true when a save happened. The
    // shell wires this to a timer; the undo journal is persisted alongside by
    // the caller (see undo.hpp).
    using AutosaveSink = std::function<void(std::span<const std::uint8_t>)>;
    bool autosaveIfDirty(const AutosaveSink& sink);

    // Structural equality (used by the save->load round-trip tests): meshes
    // compared by their canonical indexed form, plus parameters and bake
    // state. The dirty flag is not part of identity.
    friend bool operator==(const Document& a, const Document& b);

private:
    bool m_dirty = false;
};

}  // namespace cyber::app
