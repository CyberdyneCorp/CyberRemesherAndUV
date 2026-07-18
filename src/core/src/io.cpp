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
    const std::string ext = detail::lowercaseExtension(path);
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
    return Error{ErrorCode::UnsupportedFormat,
                 "unsupported import format '" + ext + "' for '" + path.string() + "'"};
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
    return Error{ErrorCode::UnsupportedFormat,
                 "unsupported export format '" + ext + "' for '" + path.string() + "'"};
}

}  // namespace cyber::io
