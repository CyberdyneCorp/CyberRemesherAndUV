#pragma once

#include <cstdint>
#include <functional>
#include <optional>
#include <span>
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
namespace cyber::app {

enum class BakeMapKind : std::uint32_t {
    Normal,
    AmbientOcclusion,
    Displacement,
    Position,
    Color,
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

    // ---- serialization (task 8.1) -------------------------------------
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
