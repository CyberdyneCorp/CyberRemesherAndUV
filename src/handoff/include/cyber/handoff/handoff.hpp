#pragma once

#include <cstddef>
#include <cstdint>
#include <filesystem>
#include <iosfwd>
#include <string>
#include <string_view>
#include <vector>

#include "cyber/core/io.hpp"
#include "cyber/core/mesh.hpp"

// Sculpt handoff ingest (pipeline-bridge spec, "Sculpt handoff ingest"). Reads
// a versioned sculpt handoff — positions, per-vertex normals, vertex colors and
// a material-mix weight — as a Target for remeshing, UV and baking.
//
// The handoff is an INTERCHANGE, not a coupling: this module knows nothing
// about which engine produced the data, links nothing beyond cyber_core, and
// gates compatibility on a declared version rather than on a producer name. The
// normative format lives in docs/sculpt-handoff-format.md.
namespace cyber::handoff {

// Supported handoff version. Compatibility rule: same major and minor <= these
// is accepted; anything else is io::ErrorCode::IncompatibleVersion naming both
// versions. A NEWER minor is rejected rather than read with the unknown parts
// dropped — silently ignoring a field the producer added to change the result
// is exactly what the version exists to prevent.
inline constexpr int kVersionMajor = 1;
inline constexpr int kVersionMinor = 0;

struct Version {
    int major = kVersionMajor;
    int minor = kVersionMinor;
};

// Per-vertex material blend weight in [0,1]. The engine carries it; it does not
// interpret it. Lives on the VERTEX attribute set, alongside io::kColorAttribute
// and the per-vertex io::kNormalAttribute (which shares its name with the
// engine's per-CORNER normal attribute but not its attribute set).
inline constexpr const char* kMaterialMixAttribute = "material_mix";

struct Handoff {
    Version version;
    std::string producer;  // free-form label from the producer, may be empty
    Mesh mesh;
    Vec3 boundsMin{};
    Vec3 boundsMax{};
    std::vector<std::string> warnings;
    // Triangles the file or buffer described that the Mesh refused (a repeated
    // vertex index, the routine degenerate in a sculpt export). Ingest keeps
    // the rest, but the loss is counted and warned about — never silent.
    std::size_t droppedFaces = 0;

    [[nodiscard]] bool hasVertexColors() const {
        return mesh.vertexAttributes().find<Vec3>(io::kColorAttribute) != nullptr;
    }
    [[nodiscard]] bool hasVertexNormals() const {
        return mesh.vertexAttributes().find<Vec3>(io::kNormalAttribute) != nullptr;
    }
    [[nodiscard]] bool hasMaterialMix() const {
        return mesh.vertexAttributes().find<float>(kMaterialMixAttribute) != nullptr;
    }
};

// The in-memory handoff profile: plain arrays, no intermediate file, for
// on-device composition where both engines share a process. Optional payload
// pointers may be null; `positions` and `indices` may not.
struct BufferView {
    const float* positions = nullptr;    // 3 * vertexCount
    const float* normals = nullptr;      // 3 * vertexCount, optional
    const float* colors = nullptr;       // 3 * vertexCount in [0,1], optional
    const float* materialMix = nullptr;  // vertexCount, optional
    std::size_t vertexCount = 0;
    const std::uint32_t* indices = nullptr;  // triangles; indexCount % 3 == 0
    std::size_t indexCount = 0;
    Version version;
    const char* producer = nullptr;  // optional NUL-terminated label
};

// Reads a handoff file. Dispatches on extension: .ply is the normative profile,
// .gltf/.glb read the version from asset.extras.cyberSculptHandoff and take
// geometry through the engine's glTF importer.
[[nodiscard]] io::Result<Handoff> readFile(const std::filesystem::path& path);

// Reads the PLY handoff profile from a stream (stdin, a socket, a buffer).
// `originLabel` names the source in error messages.
[[nodiscard]] io::Result<Handoff> readStream(std::istream& stream,
                                             std::string_view originLabel = "<stream>");

// Reads the PLY handoff profile out of a contiguous byte range already in
// memory — the same bytes a file would hold.
[[nodiscard]] io::Result<Handoff> readBytes(std::string_view bytes,
                                            std::string_view originLabel = "<memory>");

// Reads the in-memory buffer profile. Subject to the same version gate as a
// file, so an on-device producer cannot bypass compatibility checking.
[[nodiscard]] io::Result<Handoff> readBuffers(const BufferView& buffers);

}  // namespace cyber::handoff
