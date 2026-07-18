#pragma once

#include <filesystem>
#include <string>
#include <variant>
#include <vector>

#include "cyber/core/mesh.hpp"

namespace cyber::io {

// Typed error model (mesh-io spec, "Loud failure semantics"): every import
// or export failure carries a code and a human-readable message naming the
// path and reason. Silent failure is prohibited by spec.
enum class ErrorCode {
    FileNotFound,
    UnsupportedFormat,
    ParseError,
    EmptyMesh,
    WriteFailed,
};

struct Error {
    ErrorCode code;
    std::string message;
};

template <typename T>
class Result {
public:
    Result(T value) : m_data(std::move(value)) {}      // NOLINT(google-explicit-constructor)
    Result(Error error) : m_data(std::move(error)) {}  // NOLINT(google-explicit-constructor)

    [[nodiscard]] bool ok() const { return std::holds_alternative<T>(m_data); }
    [[nodiscard]] T& value() { return std::get<T>(m_data); }
    [[nodiscard]] const T& value() const { return std::get<T>(m_data); }
    [[nodiscard]] const Error& error() const { return std::get<Error>(m_data); }

private:
    std::variant<T, Error> m_data;
};

struct Status {
    Status() = default;
    Status(Error error) : m_error{std::move(error)} {}  // NOLINT(google-explicit-constructor)
    [[nodiscard]] bool ok() const { return m_error.message.empty(); }
    [[nodiscard]] const Error& error() const { return m_error; }

private:
    Error m_error{ErrorCode::WriteFailed, ""};
};

// Names of the well-known attributes importers fill and exporters read.
inline constexpr const char* kColorAttribute = "color";    // per-vertex Vec3
inline constexpr const char* kUvAttribute = "uv";          // per-corner Vec2
inline constexpr const char* kNormalAttribute = "normal";  // per-corner Vec3

enum class PolygonPolicy { Preserve, Triangulate };

struct ImportOptions {
    PolygonPolicy polygons = PolygonPolicy::Preserve;
};

struct ImportedMesh {
    Mesh mesh;
    Vec3 boundsMin, boundsMax;          // mesh-io spec, "Import scale and unit sanity"
    std::vector<std::string> warnings;  // skipped degenerate faces etc.
};

// Dispatches on file extension (.obj today; .ply/.stl/.gltf/.glb are task
// group 3.2). Reads positions, face connectivity with exact arity (n-gons
// are never mis-strided — mesh-io spec, "Correct polygon handling"), vertex
// colors (including polypaint-style xyzrgb), corner UVs and normals.
[[nodiscard]] Result<ImportedMesh> importMesh(const std::filesystem::path& path,
                                              const ImportOptions& options = {});

struct ExportOptions {
    bool writeNormals = true;
    bool writeUvs = true;
    bool writeMaterialFile = true;  // OBJ: sibling .mtl referenced via mtllib
};

// Exports by extension (.obj today). Quads and n-gons keep their arity.
[[nodiscard]] Status exportMesh(const Mesh& mesh, const std::filesystem::path& path,
                                const ExportOptions& options = {});

}  // namespace cyber::io
