#include <cctype>
#include <limits>

#include "io_internal.hpp"

namespace cyber::io {

namespace detail {

std::string lowercaseExtension(const std::filesystem::path& path) {
    std::string ext = path.extension().string();
    for (char& c : ext) {
        c = static_cast<char>(std::tolower(static_cast<unsigned char>(c)));
    }
    return ext;
}

void computeBounds(ImportedMesh& imported) {
    constexpr float kMax = std::numeric_limits<float>::max();
    imported.boundsMin = {kMax, kMax, kMax};
    imported.boundsMax = {-kMax, -kMax, -kMax};
    for (Index i = 0; i < imported.mesh.vertexCapacity(); ++i) {
        if (imported.mesh.isAlive(VertexId{i})) {
            imported.boundsMin = min(imported.boundsMin, imported.mesh.position(VertexId{i}));
            imported.boundsMax = max(imported.boundsMax, imported.mesh.position(VertexId{i}));
        }
    }
}

}  // namespace detail

Result<ImportedMesh> importMesh(const std::filesystem::path& path, const ImportOptions& options) {
    std::error_code ec;
    if (!std::filesystem::exists(path, ec)) {
        return Error{ErrorCode::FileNotFound, "no such file: '" + path.string() + "'"};
    }
    if (options.maxInputBytes > 0) {
        const std::uintmax_t bytes = std::filesystem::file_size(path, ec);
        if (ec) {
            return Error{ErrorCode::ParseError,
                         "cannot determine size of '" + path.string() + "': " + ec.message()};
        }
        if (bytes > options.maxInputBytes) {
            return Error{ErrorCode::ResourceLimit,
                         "input '" + path.string() + "' is " + std::to_string(bytes) +
                             " bytes, over this host's input budget of " +
                             std::to_string(options.maxInputBytes)};
        }
    }
    const std::string ext = detail::lowercaseExtension(path);
    Result<ImportedMesh> imported = [&]() -> Result<ImportedMesh> {
        if (ext == ".obj") {
            return detail::importObj(path, options);
        }
        if (ext == ".stl") {
            return detail::importStl(path, options);
        }
        if (ext == ".ply") {
            return detail::importPly(path, options);
        }
        if (ext == ".gltf" || ext == ".glb") {
            return detail::importGltf(path, options);
        }
        if (ext == ".fbx") {
            return detail::importFbx(path, options);
        }
        return Error{ErrorCode::UnsupportedFormat,
                     "unsupported import format '" + ext + "' for '" + path.string() + "'"};
    }();

    // The resource ceiling, applied once here rather than in five importers.
    //
    // It bounds the RESULT, not the peak: the file is fully parsed before this
    // refuses it. That is deliberate and worth stating plainly rather than
    // overclaiming, because loading is the cheap half -- what a host actually
    // needs is to refuse a mesh before the REMESHING pipeline spends orders of
    // magnitude more on it, and this is the boundary where that decision
    // belongs. Peak-bounded parsing is a per-format concern, and the formats
    // that declare their counts (PLY, binary STL, glTF accessors) already check
    // those declarations against the bytes present, which is the HOSTILITY
    // bound and a different quantity from this one.
    if (options.maxVertices > 0 && imported.ok() &&
        imported.value().mesh.vertexCount() > options.maxVertices) {
        return Error{ErrorCode::ResourceLimit,
                     "'" + path.string() + "' carries " +
                         std::to_string(imported.value().mesh.vertexCount()) +
                         " vertices, over this host's ceiling of " +
                         std::to_string(options.maxVertices)};
    }
    if (options.maxFaces > 0 && imported.ok() &&
        imported.value().mesh.faceCount() > options.maxFaces) {
        return Error{ErrorCode::ResourceLimit,
                     "'" + path.string() + "' carries " +
                         std::to_string(imported.value().mesh.faceCount()) +
                         " faces, over this host's ceiling of " +
                         std::to_string(options.maxFaces)};
    }
    return imported;
}

Status exportMesh(const Mesh& mesh, const std::filesystem::path& path,
                  const ExportOptions& options) {
    if (mesh.faceCount() == 0) {
        return Error{ErrorCode::EmptyMesh, "refusing to export an empty mesh"};
    }
    const std::string ext = detail::lowercaseExtension(path);
    if (ext == ".obj") {
        return detail::exportObj(mesh, path, options);
    }
    if (ext == ".stl") {
        return detail::exportStl(mesh, path, options);
    }
    if (ext == ".ply") {
        return detail::exportPly(mesh, path, options);
    }
    if (ext == ".gltf" || ext == ".glb") {
        return detail::exportGltf(mesh, path, options);
    }
    if (ext == ".fbx") {
        // Named separately from the generic message below: "unsupported" reads
        // like a bug when the same extension imports fine (mesh-io spec,
        // "Export formats" — FBX export is refused with an actionable error).
        return Error{ErrorCode::UnsupportedFormat,
                     "FBX is import-only: cannot write '" + path.string() +
                         "'. Export to .obj, .ply, .stl, .gltf or .glb instead"};
    }
    return Error{ErrorCode::UnsupportedFormat,
                 "unsupported export format '" + ext + "' for '" + path.string() + "'"};
}

}  // namespace cyber::io
