#include <fstream>

#include "io_internal.hpp"

#define TINYOBJLOADER_IMPLEMENTATION
#include <tiny_obj_loader.h>

namespace cyber::io::detail {

Result<ImportedMesh> importObj(const std::filesystem::path& path, const ImportOptions& options) {
    tinyobj::ObjReaderConfig config;
    config.triangulate = false;  // arity preserved; policy applied afterwards
    config.vertex_color = true;

    tinyobj::ObjReader reader;
    if (!reader.ParseFromFile(path.string(), config)) {
        return Error{ErrorCode::ParseError,
                     "failed to parse '" + path.string() + "': " + reader.Error()};
    }

    const tinyobj::attrib_t& attrib = reader.GetAttrib();
    ImportedMesh out;
    std::vector<VertexId> vertexIds;
    const std::size_t vertexCount = attrib.vertices.size() / 3;
    vertexIds.reserve(vertexCount);
    for (std::size_t i = 0; i < vertexCount; ++i) {
        out.mesh.addVertex(
            {attrib.vertices[3 * i], attrib.vertices[3 * i + 1], attrib.vertices[3 * i + 2]});
        vertexIds.push_back({static_cast<Index>(i)});
    }

    // Vertex colors: tinyobjloader fills 1,1,1 defaults; only create the
    // attribute when the file actually carried colors.
    if (attrib.colors.size() == attrib.vertices.size()) {
        bool anyColor = false;
        for (std::size_t i = 0; i < attrib.colors.size(); i += 3) {
            if (attrib.colors[i] != 1.0f || attrib.colors[i + 1] != 1.0f ||
                attrib.colors[i + 2] != 1.0f) {
                anyColor = true;
                break;
            }
        }
        if (anyColor) {
            auto& colors = out.mesh.vertexAttributes().create<Vec3>(kColorAttribute);
            for (std::size_t i = 0; i < vertexCount; ++i) {
                colors[i] = {attrib.colors[3 * i], attrib.colors[3 * i + 1],
                             attrib.colors[3 * i + 2]};
            }
        }
    }

    const bool hasUvs = !attrib.texcoords.empty();
    const bool hasNormals = !attrib.normals.empty();
    std::size_t skipped = 0;
    std::vector<VertexId> faceVerts;

    // Create the UV/normal corner columns ONCE. addFace grows every existing corner
    // column, so the columns track the corner count; the per-face loop below just
    // re-fetches the (possibly reallocated) pointer with the cheap find(). Calling
    // create() per face instead reconstructs a size-m_size temporary every time —
    // O(faces * corners) = O(n^2), ~126 s on a 554k-face mesh.
    if (hasUvs) {
        out.mesh.cornerAttributes().create<Vec2>(kUvAttribute);
    }
    if (hasNormals) {
        out.mesh.cornerAttributes().create<Vec3>(kNormalAttribute);
    }

    for (const tinyobj::shape_t& shape : reader.GetShapes()) {
        std::size_t indexOffset = 0;
        for (const std::size_t fv : shape.mesh.num_face_vertices) {
            faceVerts.clear();
            for (std::size_t v = 0; v < fv; ++v) {
                const tinyobj::index_t idx = shape.mesh.indices[indexOffset + v];
                if (idx.vertex_index < 0 ||
                    static_cast<std::size_t>(idx.vertex_index) >= vertexCount) {
                    faceVerts.clear();
                    break;
                }
                faceVerts.push_back(vertexIds[static_cast<std::size_t>(idx.vertex_index)]);
            }
            const FaceId f = faceVerts.size() >= 3 ? out.mesh.addFace(faceVerts) : FaceId{};
            if (!f.valid()) {
                ++skipped;
                indexOffset += fv;
                continue;
            }

            if (hasUvs || hasNormals) {
                // Cheap re-fetch (map lookup, no allocation); addFace above may have
                // reallocated the columns as it grew them for this face's corners.
                auto* uvs = hasUvs ? out.mesh.cornerAttributes().find<Vec2>(kUvAttribute) : nullptr;
                auto* normals =
                    hasNormals ? out.mesh.cornerAttributes().find<Vec3>(kNormalAttribute) : nullptr;
                const std::vector<LoopId> loops = out.mesh.faceLoops(f);
                for (std::size_t v = 0; v < fv; ++v) {
                    const tinyobj::index_t idx = shape.mesh.indices[indexOffset + v];
                    if (uvs && idx.texcoord_index >= 0) {
                        const auto t = static_cast<std::size_t>(idx.texcoord_index);
                        (*uvs)[loops[v].value] = {attrib.texcoords[2 * t],
                                                  attrib.texcoords[2 * t + 1]};
                    }
                    if (normals && idx.normal_index >= 0) {
                        const auto n = static_cast<std::size_t>(idx.normal_index);
                        (*normals)[loops[v].value] = {attrib.normals[3 * n],
                                                      attrib.normals[3 * n + 1],
                                                      attrib.normals[3 * n + 2]};
                    }
                }
            }
            if (options.polygons == PolygonPolicy::Triangulate && fv > 3) {
                out.mesh.triangulateFace(f);
            }
            indexOffset += fv;
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
}

Status exportObj(const Mesh& mesh, const std::filesystem::path& path,
                 const ExportOptions& options) {
    std::ofstream file(path, std::ios::trunc);
    if (!file) {
        return Error{ErrorCode::WriteFailed, "cannot open '" + path.string() + "' for writing"};
    }

    file << "# CyberRemesher\n";
    if (options.writeMaterialFile) {
        const std::filesystem::path mtlPath = std::filesystem::path(path).replace_extension(".mtl");
        std::ofstream mtl(mtlPath, std::ios::trunc);
        if (mtl) {
            mtl << "newmtl default\nKd 0.8 0.8 0.8\n";
            file << "mtllib " << mtlPath.filename().string() << "\nusemtl default\n";
        }
    }

    // Vertices (with colors when present), remapped to a compact 1-based
    // index space in id order — matching toIndexed()'s deterministic order.
    const auto* colors = mesh.vertexAttributes().find<Vec3>(kColorAttribute);
    std::vector<Index> remap(mesh.vertexCapacity(), kInvalidIndex);
    Index next = 1;
    for (Index i = 0; i < mesh.vertexCapacity(); ++i) {
        if (!mesh.isAlive(VertexId{i})) {
            continue;
        }
        remap[i] = next++;
        const Vec3 p = mesh.position(VertexId{i});
        file << "v " << p.x << ' ' << p.y << ' ' << p.z;
        if (colors) {
            const Vec3 c = (*colors)[i];
            file << ' ' << c.x << ' ' << c.y << ' ' << c.z;
        }
        file << '\n';
    }

    const auto* uvs = options.writeUvs ? mesh.cornerAttributes().find<Vec2>(kUvAttribute) : nullptr;
    const auto* normals =
        options.writeNormals ? mesh.cornerAttributes().find<Vec3>(kNormalAttribute) : nullptr;

    // Per-corner vt/vn records, one per loop, in face-iteration order.
    std::vector<Index> uvIndex(mesh.loopCapacity(), kInvalidIndex);
    std::vector<Index> normalIndex(mesh.loopCapacity(), kInvalidIndex);
    Index nextUv = 1, nextNormal = 1;
    for (Index fi = 0; fi < mesh.faceCapacity(); ++fi) {
        if (!mesh.isAlive(FaceId{fi})) {
            continue;
        }
        for (const LoopId l : mesh.faceLoops(FaceId{fi})) {
            if (uvs) {
                const Vec2 t = (*uvs)[l.value];
                file << "vt " << t.x << ' ' << t.y << '\n';
                uvIndex[l.value] = nextUv++;
            }
            if (normals) {
                const Vec3 n = (*normals)[l.value];
                file << "vn " << n.x << ' ' << n.y << ' ' << n.z << '\n';
                normalIndex[l.value] = nextNormal++;
            }
        }
    }

    for (Index fi = 0; fi < mesh.faceCapacity(); ++fi) {
        if (!mesh.isAlive(FaceId{fi})) {
            continue;
        }
        file << 'f';
        for (const LoopId l : mesh.faceLoops(FaceId{fi})) {
            const Index v = remap[mesh.loopVertex(l).value];
            file << ' ' << v;
            const bool hasUv = uvs && uvIndex[l.value] != kInvalidIndex;
            const bool hasNormal = normals && normalIndex[l.value] != kInvalidIndex;
            if (hasUv && hasNormal) {
                file << '/' << uvIndex[l.value] << '/' << normalIndex[l.value];
            } else if (hasUv) {
                file << '/' << uvIndex[l.value];
            } else if (hasNormal) {
                file << "//" << normalIndex[l.value];
            }
        }
        file << '\n';
    }

    file.flush();
    if (!file) {
        return Error{ErrorCode::WriteFailed, "write to '" + path.string() + "' failed"};
    }
    return {};
}

}  // namespace cyber::io::detail
