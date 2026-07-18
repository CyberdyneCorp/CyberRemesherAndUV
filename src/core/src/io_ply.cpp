#include <happly.h>

#include <algorithm>
#include <exception>

#include "io_internal.hpp"

namespace cyber::io::detail {

Result<ImportedMesh> importPly(const std::filesystem::path& path, const ImportOptions& options) {
    try {
        happly::PLYData ply(path.string());
        ply.validate();

        const std::vector<std::array<double, 3>> positions = ply.getVertexPositions();
        std::vector<std::vector<std::size_t>> faces = ply.getFaceIndices<std::size_t>();

        ImportedMesh out;
        std::vector<VertexId> ids;
        ids.reserve(positions.size());
        for (const auto& p : positions) {
            ids.push_back(out.mesh.addVertex(
                {static_cast<float>(p[0]), static_cast<float>(p[1]), static_cast<float>(p[2])}));
        }

        if (ply.getElement("vertex").hasProperty("red")) {
            const std::vector<std::array<unsigned char, 3>> colors = ply.getVertexColors();
            auto& column = out.mesh.vertexAttributes().create<Vec3>(kColorAttribute);
            for (std::size_t i = 0; i < colors.size() && i < ids.size(); ++i) {
                column[ids[i].value] = {static_cast<float>(colors[i][0]) / 255.0f,
                                        static_cast<float>(colors[i][1]) / 255.0f,
                                        static_cast<float>(colors[i][2]) / 255.0f};
            }
        }

        std::size_t skipped = 0;
        std::vector<VertexId> faceVerts;
        for (const auto& face : faces) {
            faceVerts.clear();
            bool ok = face.size() >= 3;
            for (const std::size_t i : face) {
                if (i >= ids.size()) {
                    ok = false;
                    break;
                }
                faceVerts.push_back(ids[i]);
            }
            const FaceId f = ok ? out.mesh.addFace(faceVerts) : FaceId{};
            if (!f.valid()) {
                ++skipped;
                continue;
            }
            if (options.polygons == PolygonPolicy::Triangulate && faceVerts.size() > 3) {
                out.mesh.triangulateFace(f);
            }
        }

        if (out.mesh.faceCount() == 0) {
            return Error{ErrorCode::EmptyMesh, "no usable faces in '" + path.string() + "'"};
        }
        if (skipped > 0) {
            out.warnings.push_back("skipped " + std::to_string(skipped) + " degenerate face(s)");
        }
        computeBounds(out);
        return out;
    } catch (const std::exception& e) {
        return Error{ErrorCode::ParseError, "failed to parse '" + path.string() + "': " + e.what()};
    }
}

Status exportPly(const Mesh& mesh, const std::filesystem::path& path,
                 const ExportOptions& /*options*/) {
    try {
        std::vector<Vec3> positions;
        std::vector<std::vector<Index>> faces;
        mesh.toIndexed(positions, faces);

        std::vector<std::array<double, 3>> plyPositions;
        plyPositions.reserve(positions.size());
        for (const Vec3& p : positions) {
            plyPositions.push_back({p.x, p.y, p.z});
        }
        std::vector<std::vector<std::size_t>> plyFaces;
        plyFaces.reserve(faces.size());
        for (const auto& f : faces) {
            plyFaces.emplace_back(f.begin(), f.end());
        }

        happly::PLYData ply;
        ply.addVertexPositions(plyPositions);
        ply.addFaceIndices(plyFaces);

        if (const auto* colors = mesh.vertexAttributes().find<Vec3>(kColorAttribute)) {
            std::vector<std::array<unsigned char, 3>> plyColors;
            plyColors.reserve(positions.size());
            for (Index i = 0; i < mesh.vertexCapacity(); ++i) {
                if (!mesh.isAlive(VertexId{i})) {
                    continue;
                }
                const Vec3 c = (*colors)[i];
                auto clamp255 = [](float v) {
                    return static_cast<unsigned char>(std::clamp(v, 0.0f, 1.0f) * 255.0f + 0.5f);
                };
                plyColors.push_back({{clamp255(c.x), clamp255(c.y), clamp255(c.z)}});
            }
            ply.addVertexColors(plyColors);
        }

        ply.write(path.string(), happly::DataFormat::Binary);
        return {};
    } catch (const std::exception& e) {
        return Error{ErrorCode::WriteFailed,
                     "write to '" + path.string() + "' failed: " + e.what()};
    }
}

}  // namespace cyber::io::detail
