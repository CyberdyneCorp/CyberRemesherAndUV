#pragma once

// Per-format implementations behind io.hpp's extension dispatch. Each
// translation unit owns one format (and its vendored library, if any).
#include "cyber/core/io.hpp"

namespace cyber::io::detail {

Result<ImportedMesh> importObj(const std::filesystem::path& path, const ImportOptions& options);
Status exportObj(const Mesh& mesh, const std::filesystem::path& path, const ExportOptions& options);

Result<ImportedMesh> importStl(const std::filesystem::path& path, const ImportOptions& options);
Status exportStl(const Mesh& mesh, const std::filesystem::path& path, const ExportOptions& options);

Result<ImportedMesh> importPly(const std::filesystem::path& path, const ImportOptions& options);
Status exportPly(const Mesh& mesh, const std::filesystem::path& path, const ExportOptions& options);

Result<ImportedMesh> importGltf(const std::filesystem::path& path, const ImportOptions& options);
Status exportGltf(const Mesh& mesh, const std::filesystem::path& path,
                  const ExportOptions& options);

// Shared helpers.
std::string lowercaseExtension(const std::filesystem::path& path);
void computeBounds(ImportedMesh& imported);

}  // namespace cyber::io::detail
