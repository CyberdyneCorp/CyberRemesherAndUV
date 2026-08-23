#include <ufbx.h>

#include <algorithm>
#include <cmath>
#include <cstddef>
#include <cstdint>
#include <string>
#include <vector>

#include "io_internal.hpp"

namespace cyber::io::detail {

namespace {

Vec3 toVec3(ufbx_vec3 v) {
    return {static_cast<float>(v.x), static_cast<float>(v.y), static_cast<float>(v.z)};
}

// Frees the scene on every exit path (ufbx_load_file returns an owning pointer
// and the import below has several early returns).
class SceneHandle {
public:
    explicit SceneHandle(ufbx_scene* scene) : m_scene(scene) {}
    ~SceneHandle() {
        if (m_scene != nullptr) {
            ufbx_free_scene(m_scene);
        }
    }
    SceneHandle(const SceneHandle&) = delete;
    SceneHandle& operator=(const SceneHandle&) = delete;

    [[nodiscard]] const ufbx_scene* get() const { return m_scene; }

private:
    ufbx_scene* m_scene;
};

ufbx_load_opts importOpts() {
    ufbx_load_opts opts = {};
    // Normalize the file's conventions to the engine frame: right-handed Y-up,
    // one unit = one metre (the frame the glTF importer already produces), so a
    // Z-up centimetre export arrives at the size and orientation of the same
    // model saved as OBJ.
    //
    // The unit conversion is not optional cosmetics. FBX splits scale between
    // the geometry and the node that instances it: Blender writes a 2 m cube as
    // geometry of +/-1 under a node scaled by 100, and declares one unit to be
    // a centimetre. Reading the positions without the unit conversion yields a
    // 200-unit cube — 100x the OBJ of the same model.
    //
    // MODIFY_GEOMETRY bakes the conversion into the positions instead of
    // parking it on a root node that every consumer would have to apply.
    opts.target_axes = ufbx_axes_right_handed_y_up;
    opts.target_unit_meters = 1.0;
    opts.space_conversion = UFBX_SPACE_CONVERSION_MODIFY_GEOMETRY;
    // Geometry only: no animation curves, no embedded textures, and never
    // follow a reference out to another file on disk during an import.
    opts.ignore_animation = true;
    opts.ignore_embedded = true;
    opts.load_external_files = false;
    return opts;
}

std::string formatError(const ufbx_error& error) {
    char buffer[512];
    ufbx_format_error(buffer, sizeof(buffer), &error);
    return std::string(buffer);
}

// One FBX mesh node instance appended to the destination mesh. FBX stores
// geometry once per mesh and positions it through the nodes that instance it,
// so the same ufbx_mesh may be emitted several times under different
// transforms.
class InstanceImporter {
public:
    InstanceImporter(ImportedMesh& out, const ImportOptions& options)
        : m_out(out), m_options(options) {}

    void add(const ufbx_mesh& mesh, const ufbx_matrix& geometryToWorld);

    [[nodiscard]] std::size_t skipped() const { return m_skipped; }

private:
    void writeCorners(const ufbx_mesh& mesh, ufbx_face face, FaceId f,
                      const ufbx_matrix& normalMatrix, const std::vector<VertexId>& vertexIds);

    ImportedMesh& m_out;
    const ImportOptions& m_options;
    std::size_t m_skipped = 0;
};

void InstanceImporter::add(const ufbx_mesh& mesh, const ufbx_matrix& geometryToWorld) {
    // Normals transform by the inverse-transpose, not by the position matrix,
    // so a non-uniformly scaled node keeps them perpendicular to the surface.
    const ufbx_matrix normalMatrix = ufbx_matrix_for_normals(&geometryToWorld);

    std::vector<VertexId> vertexIds;
    vertexIds.reserve(mesh.num_vertices);
    for (std::size_t i = 0; i < mesh.num_vertices; ++i) {
        vertexIds.push_back(m_out.mesh.addVertex(
            toVec3(ufbx_transform_position(&geometryToWorld, mesh.vertices.data[i]))));
    }

    std::vector<VertexId> faceVerts;
    for (std::size_t fi = 0; fi < mesh.num_faces; ++fi) {
        const ufbx_face face = mesh.faces.data[fi];
        faceVerts.clear();
        for (std::uint32_t k = 0; k < face.num_indices; ++k) {
            faceVerts.push_back(vertexIds[mesh.vertex_indices.data[face.index_begin + k]]);
        }
        // FBX admits point and line "faces" (ufbx reports them as
        // num_point_faces / num_line_faces); they carry no surface.
        const FaceId f = faceVerts.size() >= 3 ? m_out.mesh.addFace(faceVerts) : FaceId{};
        if (!f.valid()) {
            ++m_skipped;
            continue;
        }
        writeCorners(mesh, face, f, normalMatrix, vertexIds);
        if (m_options.polygons == PolygonPolicy::Triangulate && face.num_indices > 3) {
            // After the corner write, so the children inherit UVs and normals.
            m_out.mesh.triangulateFace(f);
        }
    }
}

void InstanceImporter::writeCorners(const ufbx_mesh& mesh, ufbx_face face, FaceId f,
                                    const ufbx_matrix& normalMatrix,
                                    const std::vector<VertexId>& vertexIds) {
    // Cheap re-fetch per face: addFace grows every corner column, which may
    // reallocate it (see the same note in the OBJ importer — creating the
    // columns per face instead is O(faces * corners)).
    auto* uvs =
        mesh.vertex_uv.exists ? m_out.mesh.cornerAttributes().find<Vec2>(kUvAttribute) : nullptr;
    auto* normals = mesh.vertex_normal.exists
                        ? m_out.mesh.cornerAttributes().find<Vec3>(kNormalAttribute)
                        : nullptr;
    auto* colors = mesh.vertex_color.exists
                       ? m_out.mesh.vertexAttributes().find<Vec3>(kColorAttribute)
                       : nullptr;
    if (uvs == nullptr && normals == nullptr && colors == nullptr) {
        return;
    }

    const std::vector<LoopId> loops = m_out.mesh.faceLoops(f);
    const std::size_t corners = std::min<std::size_t>(loops.size(), face.num_indices);
    for (std::size_t k = 0; k < corners; ++k) {
        const std::size_t index = face.index_begin + k;
        if (uvs != nullptr) {
            const ufbx_vec2 uv = mesh.vertex_uv[index];
            (*uvs)[loops[k].value] = {static_cast<float>(uv.x), static_cast<float>(uv.y)};
        }
        if (normals != nullptr) {
            // ufbx_matrix_for_normals returns the adjugate, which scales by the
            // determinant: under Blender's 100x node scale the transformed
            // normal comes back 10000 long. Only its direction is meaningful.
            (*normals)[loops[k].value] = normalized(
                toVec3(ufbx_transform_direction(&normalMatrix, mesh.vertex_normal[index])));
        }
        if (colors != nullptr) {
            // The engine's color attribute is per-vertex while FBX stores it
            // per corner; corners of one vertex agree in every file that did
            // not intend a hard color seam.
            const ufbx_vec4 c = mesh.vertex_color[index];
            (*colors)[vertexIds[mesh.vertex_indices.data[index]].value] = {
                static_cast<float>(c.x), static_cast<float>(c.y), static_cast<float>(c.z)};
        }
    }
}

// Corner/vertex columns are created once for the whole scene, before any face
// exists, so the per-face writes above only ever have to find() them.
void createAttributeColumns(ImportedMesh& out, const ufbx_scene& scene) {
    bool uvs = false, normals = false, colors = false;
    for (std::size_t i = 0; i < scene.meshes.count; ++i) {
        const ufbx_mesh& mesh = *scene.meshes.data[i];
        uvs = uvs || mesh.vertex_uv.exists;
        normals = normals || mesh.vertex_normal.exists;
        colors = colors || mesh.vertex_color.exists;
    }
    if (uvs) {
        out.mesh.cornerAttributes().create<Vec2>(kUvAttribute);
    }
    if (normals) {
        out.mesh.cornerAttributes().create<Vec3>(kNormalAttribute);
    }
    if (colors) {
        out.mesh.vertexAttributes().create<Vec3>(kColorAttribute);
    }
}

}  // namespace

Result<ImportedMesh> importFbx(const std::filesystem::path& path, const ImportOptions& options) {
    const ufbx_load_opts opts = importOpts();
    ufbx_error error;
    const SceneHandle handle(ufbx_load_file(path.string().c_str(), &opts, &error));
    if (handle.get() == nullptr) {
        return Error{ErrorCode::ParseError,
                     "failed to parse '" + path.string() + "': " + formatError(error)};
    }
    const ufbx_scene& scene = *handle.get();

    ImportedMesh out;
    createAttributeColumns(out, scene);

    InstanceImporter importer(out, options);
    for (std::size_t i = 0; i < scene.meshes.count; ++i) {
        const ufbx_mesh& mesh = *scene.meshes.data[i];
        if (mesh.instances.count == 0) {
            // Geometry present in the file but attached to no node: keep it
            // rather than drop it, in its own local frame.
            importer.add(mesh, ufbx_identity_matrix);
            continue;
        }
        for (std::size_t n = 0; n < mesh.instances.count; ++n) {
            importer.add(mesh, mesh.instances.data[n]->geometry_to_world);
        }
    }

    if (out.mesh.faceCount() == 0) {
        return Error{ErrorCode::EmptyMesh, "no usable faces in '" + path.string() + "'"};
    }
    if (importer.skipped() > 0) {
        out.warnings.push_back("skipped " + std::to_string(importer.skipped()) +
                               " degenerate face(s)");
    }
    computeBounds(out);
    return out;
}

}  // namespace cyber::io::detail
