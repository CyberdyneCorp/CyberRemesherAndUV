// CyberRemesher C ABI implementation (capi module, task 13.1).
//
// Bridges the pure-C surface declared in cyber_capi.h onto the internal
// C++20 pipeline: opaque handles wrap cyber::Mesh, C function pointers are
// adapted into a ProgressSink / CancelToken, and every C++ error channel
// (typed io::Error, RunStatus, thrown exceptions) is funnelled into a
// CyberStatus plus a thread-local message. No exception is ever allowed to
// unwind across the C boundary.

#include "cyber_capi.h"

#include <algorithm>
#include <array>
#include <cstdint>
#include <exception>
#include <filesystem>
#include <memory>
#include <vector>
#include <optional>
#include <string>
#include <string_view>
#include <unordered_set>

#include "cyber/bake/bake.hpp"
#include "cyber/core/io.hpp"
#include "cyber/core/mesh.hpp"
#include "cyber/core/pipeline.hpp"
#include "cyber/quadrangulate/field_quadrangulator.hpp"
#include "cyber/quadrangulate/quadcover_extractor.hpp"
#include "cyber/quadrangulate/position_field.hpp"
#include "cyber/imageio/image.hpp"
#include "cyber/core/progress.hpp"
#include "cyber/core/remesh_params.hpp"
#include "cyber/retopo/actions.hpp"
#include "cyber/retopo/boundary.hpp"
#include "cyber/retopo/build_tools.hpp"
#include "cyber/retopo/commands.hpp"
#include "cyber/retopo/dissolve.hpp"
#include "cyber/retopo/erase.hpp"
#include "cyber/retopo/loop_metrics.hpp"
#include "cyber/retopo/loops.hpp"
#include "cyber/retopo/move.hpp"
#include "cyber/retopo/picking.hpp"
#include "cyber/retopo/paths.hpp"
#include "cyber/retopo/pins.hpp"
#include "cyber/retopo/relax.hpp"
#include "cyber/retopo/snapping.hpp"
#include "cyber/retopo/stroke_interpreter.hpp"
#include "cyber/retopo/symmetry.hpp"
#include "cyber/retopo/tweak.hpp"

// Version numbers are injected from the CMake project() version so this file
// stays the single C-facing mirror of the engine version.
#ifndef CYBER_CAPI_VERSION_MAJOR
#define CYBER_CAPI_VERSION_MAJOR 0
#endif
#ifndef CYBER_CAPI_VERSION_MINOR
#define CYBER_CAPI_VERSION_MINOR 0
#endif
#ifndef CYBER_CAPI_VERSION_PATCH
#define CYBER_CAPI_VERSION_PATCH 0
#endif

// Compacted render-ready buffers derived from a mesh handle (the
// cyber_mesh_*_ptr / render-data accessors). Built lazily on first access;
// the current C API never mutates an existing handle, so once built the
// cache stays valid for the handle's lifetime (see the LIFETIME note in
// cyber_capi.h — any future mutating entry point must reset `built`).
struct CyberRenderCache {
    std::vector<float> positions;        // x,y,z per vertex, compacted order
    std::vector<std::uint32_t> indices;  // 3 per triangle, deterministic fan
    std::vector<std::uint32_t> edges;    // 2 per unique undirected face edge
    std::vector<std::uint32_t> tagged;   // 2 per tagged visible edge
    std::vector<float> normals;          // x,y,z per vertex, unit length
    std::vector<float> colors;           // r,g,b per vertex; empty if none
    bool hasColors = false;
    bool built = false;
};

// Opaque handle definition: the mesh plus any statistics captured when the
// handle was produced by cyber_remesh (islandsFailed cannot be recovered
// from geometry alone), plus overlay render state (task 3.4): hidden faces
// and tagged edges filter/augment the render cache without touching the
// topology — stable element ids are unaffected. Both survive render-cache
// invalidation (they live on the handle, not in the cache) and reference
// stable ids, so dead ids are simply skipped at cache-build time.
struct CyberMesh {
    cyber::Mesh mesh;
    std::optional<cyber::remesh::Statistics> stats;
    std::unordered_set<std::uint32_t> hiddenFaces;
    std::vector<std::uint32_t> taggedEdges;
    mutable CyberRenderCache render;  // lazy; see ensureRenderCache
};

namespace {

std::string& errorSlot() {
    thread_local std::string message;
    return message;
}

void setError(std::string message) { errorSlot() = std::move(message); }
void clearError() { errorSlot().clear(); }

CyberStatus mapIoError(const cyber::io::Error& error) {
    setError(error.message);
    switch (error.code) {
        case cyber::io::ErrorCode::EmptyMesh:
            return CYBER_ERR_EMPTY;
        case cyber::io::ErrorCode::FileNotFound:
        case cyber::io::ErrorCode::UnsupportedFormat:
        case cyber::io::ErrorCode::ParseError:
        case cyber::io::ErrorCode::WriteFailed:
            break;
    }
    return CYBER_ERR_IO;
}

cyber::remesh::Parameters toParameters(const CyberRemeshParams& in) {
    cyber::remesh::Parameters params;
    params.targetQuadCount = in.targetQuads;
    params.edgeScale = in.edgeScale;
    params.sharpEdgeDegrees = in.sharpEdgeDegrees;
    params.smoothNormalDegrees = in.smoothNormalDegrees;
    params.adaptivity = in.adaptivity;
    params.pureQuads = in.pureQuads != 0;
    params.holeFillMaxBoundary = in.holeFillMaxBoundary;
    return params;
}

// Adapts the C progress/cancel callbacks into a ProgressSink. Cancellation is
// cooperative and polled on every progress report: the pipeline reports at
// each stage boundary, so a returned non-zero cancel flag flips the shared
// token and the next poll inside the pipeline aborts cleanly.
cyber::ProgressSink makeSink(CyberProgressCb progress, CyberCancelCb cancel, void* user,
                             const cyber::CancelToken& token) {
    return cyber::ProgressSink(
        [progress, cancel, user, token](float fraction, std::string_view stage) {
            if (cancel != nullptr && cancel(user) != 0) {
                token.requestCancel();
            }
            if (progress != nullptr) {
                // Null-terminate for the C callee; `stage` may be a view.
                const std::string stageStr(stage);
                progress(fraction, stageStr.c_str(), user);
            }
        });
}

}  // namespace

void cyber_version(int* major, int* minor, int* patch) {
    if (major != nullptr) {
        *major = CYBER_CAPI_VERSION_MAJOR;
    }
    if (minor != nullptr) {
        *minor = CYBER_CAPI_VERSION_MINOR;
    }
    if (patch != nullptr) {
        *patch = CYBER_CAPI_VERSION_PATCH;
    }
}

const char* cyber_status_string(CyberStatus status) {
    switch (status) {
        case CYBER_OK:
            return "ok";
        case CYBER_ERR_IO:
            return "I/O error";
        case CYBER_ERR_INVALID_ARG:
            return "invalid argument";
        case CYBER_ERR_INVALID_PARAM:
            return "invalid parameter";
        case CYBER_ERR_EMPTY:
            return "empty mesh";
        case CYBER_ERR_RUNTIME:
            return "runtime error";
        case CYBER_ERR_CANCELLED:
            return "cancelled";
    }
    return "unknown status";
}

const char* cyber_last_error(void) { return errorSlot().c_str(); }

CyberStatus cyber_mesh_load_obj(const char* path, CyberMesh** out) {
    if (path == nullptr || out == nullptr) {
        setError("cyber_mesh_load_obj: null argument");
        return CYBER_ERR_INVALID_ARG;
    }
    *out = nullptr;
    try {
        auto result = cyber::io::importMesh(std::filesystem::path(path));
        if (!result.ok()) {
            return mapIoError(result.error());
        }
        auto handle = std::make_unique<CyberMesh>();
        handle->mesh = std::move(result.value().mesh);
        clearError();
        *out = handle.release();
        return CYBER_OK;
    } catch (const std::exception& e) {
        setError(std::string("cyber_mesh_load_obj: ") + e.what());
        return CYBER_ERR_RUNTIME;
    } catch (...) {
        setError("cyber_mesh_load_obj: unknown error");
        return CYBER_ERR_RUNTIME;
    }
}

CyberStatus cyber_mesh_save_obj(const CyberMesh* mesh, const char* path) {
    if (mesh == nullptr || path == nullptr) {
        setError("cyber_mesh_save_obj: null argument");
        return CYBER_ERR_INVALID_ARG;
    }
    try {
        const cyber::io::Status status =
            cyber::io::exportMesh(mesh->mesh, std::filesystem::path(path));
        if (!status.ok()) {
            return mapIoError(status.error());
        }
        clearError();
        return CYBER_OK;
    } catch (const std::exception& e) {
        setError(std::string("cyber_mesh_save_obj: ") + e.what());
        return CYBER_ERR_RUNTIME;
    } catch (...) {
        setError("cyber_mesh_save_obj: unknown error");
        return CYBER_ERR_RUNTIME;
    }
}

void cyber_mesh_free(CyberMesh* mesh) { delete mesh; }

void cyber_default_params(CyberRemeshParams* params) {
    if (params == nullptr) {
        return;
    }
    const cyber::remesh::Parameters defaults;
    params->targetQuads = defaults.targetQuadCount;
    params->edgeScale = defaults.edgeScale;
    params->sharpEdgeDegrees = defaults.sharpEdgeDegrees;
    params->smoothNormalDegrees = defaults.smoothNormalDegrees;
    params->adaptivity = defaults.adaptivity;
    params->pureQuads = defaults.pureQuads ? 1 : 0;
    params->holeFillMaxBoundary = defaults.holeFillMaxBoundary;
    params->quadMethod = CYBER_QUAD_FIELD_ALIGNED;
}

CyberStatus cyber_remesh(const CyberMesh* in, const CyberRemeshParams* params,
                         CyberProgressCb progress, CyberCancelCb cancel, void* user,
                         CyberMesh** out) {
    if (in == nullptr || params == nullptr || out == nullptr) {
        setError("cyber_remesh: null argument");
        return CYBER_ERR_INVALID_ARG;
    }
    *out = nullptr;
    try {
        const cyber::remesh::Parameters cppParams = toParameters(*params);
        const cyber::CancelToken token;
        cyber::ProgressSink sink = makeSink(progress, cancel, user, token);

        // The field-aligned quadrangulator (maximum triangle matching over a
        // smoothed cross field) gives both the highest quad-dominance (~95%+ on
        // clean input) AND edge flow that follows curvature, so it is the
        // default for the C ABI / bindings. Pure-quad mode (params.pureQuads)
        // yields a 100%-quad result on top of it. quadMethod selects the
        // Instant-Meshes position-field extractor instead — more uniform,
        // field-aligned edge flow with fewer/better-placed singularities.
        // quadMethod selects the extractor: field-aligned (default), the
        // Instant-Meshes position-field extractor, or the integer-parametrization
        // extractor (Milestones 3-5, experimental).
        const int quadMethod = params->quadMethod;
        cyber::remesh::PipelineResult result = cyber::remesh::remesh(
            in->mesh, cppParams, &sink, &token,
            [quadMethod]() -> std::unique_ptr<cyber::remesh::IQuadrangulator> {
                if (quadMethod == CYBER_QUAD_INSTANT_MESHES) {
                    return cyber::remesh::makeInstantMeshesQuadrangulator();
                }
                if (quadMethod == CYBER_QUAD_INTEGER) {
                    return cyber::remesh::makeIntegerQuadrangulator();
                }
                if (quadMethod == CYBER_QUAD_QUADCOVER) {
                    return cyber::remesh::makeQuadCoverQuadrangulator();
                }
                return cyber::remesh::makeFieldAlignedQuadrangulator();
            });

        switch (result.status) {
            case cyber::remesh::RunStatus::Success:
            case cyber::remesh::RunStatus::Partial: {
                auto handle = std::make_unique<CyberMesh>();
                handle->mesh = std::move(result.mesh);
                handle->stats = result.stats;
                clearError();
                *out = handle.release();
                return CYBER_OK;
            }
            case cyber::remesh::RunStatus::Cancelled:
                setError("cyber_remesh: cancelled");
                return CYBER_ERR_CANCELLED;
            case cyber::remesh::RunStatus::Error:
                break;
        }

        // Error: distinguish unusable parameters from a pipeline failure.
        for (const auto& issue : result.parameterIssues) {
            if (issue.fatal) {
                setError("cyber_remesh: invalid parameter '" + issue.parameter + "': " +
                         issue.message);
                return CYBER_ERR_INVALID_PARAM;
            }
        }
        setError(result.error.empty() ? "cyber_remesh: pipeline error" : result.error);
        return CYBER_ERR_RUNTIME;
    } catch (const std::exception& e) {
        setError(std::string("cyber_remesh: ") + e.what());
        return CYBER_ERR_RUNTIME;
    } catch (...) {
        setError("cyber_remesh: unknown error");
        return CYBER_ERR_RUNTIME;
    }
}

CyberStatus cyber_mesh_stats(const CyberMesh* mesh, CyberStats* out) {
    if (mesh == nullptr || out == nullptr) {
        setError("cyber_mesh_stats: null argument");
        return CYBER_ERR_INVALID_ARG;
    }
    try {
        const cyber::Mesh& m = mesh->mesh;
        CyberStats stats{};
        stats.vertices = m.vertexCount();

        for (cyber::Index i = 0; i < m.faceCapacity(); ++i) {
            const cyber::FaceId face{i};
            if (!m.isAlive(face)) {
                continue;
            }
            switch (m.faceSize(face)) {
                case 3:
                    ++stats.triangles;
                    break;
                case 4:
                    ++stats.quads;
                    break;
                default:
                    ++stats.other;
                    break;
            }
        }

        stats.islands = m.islands().size();
        stats.islandsFailed = mesh->stats ? mesh->stats->islandsFailed : 0;

        clearError();
        *out = stats;
        return CYBER_OK;
    } catch (const std::exception& e) {
        setError(std::string("cyber_mesh_stats: ") + e.what());
        return CYBER_ERR_RUNTIME;
    } catch (...) {
        setError("cyber_mesh_stats: unknown error");
        return CYBER_ERR_RUNTIME;
    }
}

CyberMesh* cyber_mesh_create(void) {
    try {
        return new CyberMesh();
    } catch (...) {
        return nullptr;
    }
}

void cyber_mesh_destroy(CyberMesh* mesh) { delete mesh; }

size_t cyber_mesh_vertex_count(const CyberMesh* mesh) {
    return mesh == nullptr ? 0 : mesh->mesh.vertexCount();
}

size_t cyber_mesh_face_count(const CyberMesh* mesh) {
    return mesh == nullptr ? 0 : mesh->mesh.faceCount();
}

size_t cyber_mesh_copy_positions(const CyberMesh* mesh, float* out, size_t max_floats) {
    if (mesh == nullptr) {
        return 0;
    }
    std::vector<cyber::Vec3> positions;
    std::vector<std::vector<cyber::Index>> faces;
    try {
        mesh->mesh.toIndexed(positions, faces);
    } catch (...) {
        return 0;  // never let an allocation failure cross the C boundary
    }
    const size_t need = positions.size() * 3;
    if (out == nullptr) {
        return need;  // query mode
    }
    size_t written = 0;
    for (const cyber::Vec3& p : positions) {
        if (written + 3 > max_floats) {
            break;
        }
        out[written++] = p.x;
        out[written++] = p.y;
        out[written++] = p.z;
    }
    return written;
}

void cyber_remesh_params_default(CyberRemeshParams* params) { cyber_default_params(params); }

// ---- render-data accessors -------------------------------------------------

namespace {

// True when `f` is in the handle's hidden set (render-only visibility
// filter, task 3.4).
bool isHiddenFace(const CyberMesh& handle, cyber::FaceId f) {
    return handle.hiddenFaces.find(f.value) != handle.hiddenFaces.end();
}

// True when the vertex should be dropped from the render compaction: it has
// at least one incident face and every incident face is hidden (a vertex
// used only by hidden geometry must not draw as a dot).
bool isHiddenVertex(const CyberMesh& handle, cyber::VertexId v) {
    if (handle.hiddenFaces.empty()) {
        return false;
    }
    const std::vector<cyber::FaceId> faces = handle.mesh.vertexFaces(v);
    if (faces.empty()) {
        return false;
    }
    for (const cyber::FaceId f : faces) {
        if (!isHiddenFace(handle, f)) {
            return false;
        }
    }
    return true;
}

// Live, visible vertices in id order — the same compaction as
// Mesh::toIndexed and cyber_mesh_copy_positions when nothing is hidden.
// Fills cache.positions and returns the vertex-id → compact-index remap.
std::vector<cyber::Index> compactVertices(const CyberMesh& handle, CyberRenderCache& cache) {
    const cyber::Mesh& m = handle.mesh;
    std::vector<cyber::Index> remap(m.vertexCapacity(), cyber::kInvalidIndex);
    cache.positions.reserve(m.vertexCount() * 3);
    cyber::Index next = 0;
    for (cyber::Index i = 0; i < m.vertexCapacity(); ++i) {
        const cyber::VertexId v{i};
        if (!m.isAlive(v) || isHiddenVertex(handle, v)) {
            continue;
        }
        remap[i] = next++;
        const cyber::Vec3 p = m.position(v);
        cache.positions.push_back(p.x);
        cache.positions.push_back(p.y);
        cache.positions.push_back(p.z);
    }
    return remap;
}

// Deterministic fan triangulation around each live face's first corner, in
// face-id order: (v0,v1,v2), (v0,v2,v3), ... — an n-gon yields n-2 triangles.
void fanTriangulate(const CyberMesh& handle, const std::vector<cyber::Index>& remap,
                    CyberRenderCache& cache) {
    const cyber::Mesh& m = handle.mesh;
    for (cyber::Index f = 0; f < m.faceCapacity(); ++f) {
        const cyber::FaceId face{f};
        if (!m.isAlive(face) || isHiddenFace(handle, face)) {
            continue;
        }
        const std::vector<cyber::VertexId> verts = m.faceVertices(face);
        for (std::size_t k = 2; k < verts.size(); ++k) {
            cache.indices.push_back(remap[verts[0].value]);
            cache.indices.push_back(remap[verts[k - 1].value]);
            cache.indices.push_back(remap[verts[k].value]);
        }
    }
}

// Unique undirected face edges (wireframe topology, NOT the fan
// triangulation: a quad contributes its 4 boundary edges and no diagonal).
// Deterministic: faces walked in id order, each edge kept on first
// sighting, endpoints emitted in face-winding order.
void extractEdges(const CyberMesh& handle, const std::vector<cyber::Index>& remap,
                  CyberRenderCache& cache) {
    const cyber::Mesh& m = handle.mesh;
    std::unordered_set<std::uint64_t> seen;
    for (cyber::Index f = 0; f < m.faceCapacity(); ++f) {
        const cyber::FaceId face{f};
        if (!m.isAlive(face) || isHiddenFace(handle, face)) {
            continue;
        }
        const std::vector<cyber::VertexId> verts = m.faceVertices(face);
        const std::size_t n = verts.size();
        for (std::size_t k = 0; k < n; ++k) {
            const std::uint32_t a = remap[verts[k].value];
            const std::uint32_t b = remap[verts[(k + 1) % n].value];
            if (a == b || a == cyber::kInvalidIndex || b == cyber::kInvalidIndex) {
                continue;  // degenerate corner or hidden endpoint
            }
            const std::uint64_t key =
                (static_cast<std::uint64_t>(std::min(a, b)) << 32) | std::max(a, b);
            if (seen.insert(key).second) {
                cache.edges.push_back(a);
                cache.edges.push_back(b);
            }
        }
    }
}

// Compacted index pairs for the handle's tagged edges (task 3.4: loop tags
// render as a colored second pass over the wireframe). Handle order is
// preserved; dead, duplicate-endpoint, or hidden edges are skipped.
void extractTaggedEdges(const CyberMesh& handle, const std::vector<cyber::Index>& remap,
                        CyberRenderCache& cache) {
    const cyber::Mesh& m = handle.mesh;
    for (const std::uint32_t id : handle.taggedEdges) {
        const cyber::EdgeId e{id};
        if (id >= m.edgeCapacity() || !m.isAlive(e)) {
            continue;
        }
        const auto [v0, v1] = m.edgeVertices(e);
        const std::uint32_t a = remap[v0.value];
        const std::uint32_t b = remap[v1.value];
        if (a == b || a == cyber::kInvalidIndex || b == cyber::kInvalidIndex) {
            continue;
        }
        cache.tagged.push_back(a);
        cache.tagged.push_back(b);
    }
}

// Per-compact-vertex normal accumulation: imported per-corner normals when
// the attribute exists, otherwise the engine's own face normals (Newell).
std::vector<cyber::Vec3> accumulateNormals(const CyberMesh& handle,
                                           const std::vector<cyber::Index>& remap,
                                           std::size_t compactCount) {
    const cyber::Mesh& m = handle.mesh;
    std::vector<cyber::Vec3> acc(compactCount, cyber::Vec3{});
    const std::vector<cyber::Vec3>* corner =
        m.cornerAttributes().find<cyber::Vec3>(cyber::io::kNormalAttribute);
    for (cyber::Index f = 0; f < m.faceCapacity(); ++f) {
        const cyber::FaceId face{f};
        if (!m.isAlive(face) || isHiddenFace(handle, face)) {
            continue;
        }
        if (corner != nullptr) {
            for (const cyber::LoopId l : m.faceLoops(face)) {
                acc[remap[m.loopVertex(l).value]] += (*corner)[l.value];
            }
        } else {
            const cyber::Vec3 n = m.faceNormal(face);
            for (const cyber::VertexId v : m.faceVertices(face)) {
                acc[remap[v.value]] += n;
            }
        }
    }
    return acc;
}

// Normalizes the accumulated normals into the cache; a vertex with no usable
// normal (isolated vertex, fully degenerate fan) gets (0,0,1) so the buffer
// is always unit-length as documented.
void finalizeNormals(const std::vector<cyber::Vec3>& acc, CyberRenderCache& cache) {
    cache.normals.reserve(acc.size() * 3);
    for (const cyber::Vec3& n : acc) {
        const float len = cyber::length(n);
        if (len > 0.0f) {
            cache.normals.push_back(n.x / len);
            cache.normals.push_back(n.y / len);
            cache.normals.push_back(n.z / len);
        } else {
            cache.normals.push_back(0.0f);
            cache.normals.push_back(0.0f);
            cache.normals.push_back(1.0f);
        }
    }
}

// Copies the per-vertex color attribute (if any) in compacted vertex order
// (the same visibility filter as compactVertices, so the streams align).
void extractColors(const CyberMesh& handle, const std::vector<cyber::Index>& remap,
                   CyberRenderCache& cache) {
    const cyber::Mesh& m = handle.mesh;
    const std::vector<cyber::Vec3>* colors =
        m.vertexAttributes().find<cyber::Vec3>(cyber::io::kColorAttribute);
    if (colors == nullptr) {
        return;
    }
    cache.hasColors = true;
    cache.colors.reserve(m.vertexCount() * 3);
    for (cyber::Index i = 0; i < m.vertexCapacity(); ++i) {
        if (!m.isAlive(cyber::VertexId{i}) || remap[i] == cyber::kInvalidIndex) {
            continue;
        }
        const cyber::Vec3 c = (*colors)[i];
        cache.colors.push_back(c.x);
        cache.colors.push_back(c.y);
        cache.colors.push_back(c.z);
    }
}

// Builds the handle's render cache once. Returns NULL when `mesh` is NULL or
// the build failed (allocation failure must not cross the C boundary).
const CyberRenderCache* ensureRenderCache(const CyberMesh* mesh) {
    if (mesh == nullptr) {
        return nullptr;
    }
    CyberRenderCache& cache = mesh->render;
    if (cache.built) {
        return &cache;
    }
    try {
        const std::vector<cyber::Index> remap = compactVertices(*mesh, cache);
        fanTriangulate(*mesh, remap, cache);
        extractEdges(*mesh, remap, cache);
        extractTaggedEdges(*mesh, remap, cache);
        finalizeNormals(accumulateNormals(*mesh, remap, cache.positions.size() / 3), cache);
        extractColors(*mesh, remap, cache);
        cache.built = true;
        return &cache;
    } catch (...) {
        cache = CyberRenderCache{};
        return nullptr;
    }
}

// Query-or-copy helper shared by the copy accessors (out=NULL queries the
// required count).
template <typename T>
size_t copyBuffer(const std::vector<T>& src, T* out, size_t max_values) {
    if (out == nullptr) {
        return src.size();
    }
    const size_t n = std::min(src.size(), max_values);
    std::copy_n(src.data(), n, out);
    return n;
}

// Pointer-view helper shared by the *_ptr accessors: NULL `src` (no cache)
// and empty buffers both surface as NULL with *out_count = 0.
template <typename T>
const T* bufferView(const std::vector<T>* src, size_t* out_count) {
    const size_t count = src == nullptr ? 0 : src->size();
    if (out_count != nullptr) {
        *out_count = count;
    }
    return count == 0 ? nullptr : src->data();
}

}  // namespace

size_t cyber_mesh_triangle_count(const CyberMesh* mesh) {
    const CyberRenderCache* cache = ensureRenderCache(mesh);
    return cache == nullptr ? 0 : cache->indices.size() / 3;
}

size_t cyber_mesh_copy_triangle_indices(const CyberMesh* mesh, uint32_t* out,
                                        size_t max_indices) {
    const CyberRenderCache* cache = ensureRenderCache(mesh);
    if (cache == nullptr) {
        return 0;
    }
    // Whole triangles only: round the writable capacity down to a multiple
    // of 3 so a truncated copy never ends mid-triangle.
    return copyBuffer(cache->indices, out, max_indices - max_indices % 3);
}

size_t cyber_mesh_edge_count(const CyberMesh* mesh) {
    const CyberRenderCache* cache = ensureRenderCache(mesh);
    return cache == nullptr ? 0 : cache->edges.size() / 2;
}

size_t cyber_mesh_copy_edge_indices(const CyberMesh* mesh, uint32_t* out, size_t max_indices) {
    const CyberRenderCache* cache = ensureRenderCache(mesh);
    if (cache == nullptr) {
        return 0;
    }
    // Whole edges only: never end a truncated copy mid-edge.
    return copyBuffer(cache->edges, out, max_indices - max_indices % 2);
}

size_t cyber_mesh_copy_normals(const CyberMesh* mesh, float* out, size_t max_floats) {
    const CyberRenderCache* cache = ensureRenderCache(mesh);
    return cache == nullptr ? 0 : copyBuffer(cache->normals, out, max_floats);
}

int cyber_mesh_has_colors(const CyberMesh* mesh) {
    const CyberRenderCache* cache = ensureRenderCache(mesh);
    return cache != nullptr && cache->hasColors ? 1 : 0;
}

size_t cyber_mesh_copy_colors(const CyberMesh* mesh, float* out, size_t max_floats) {
    const CyberRenderCache* cache = ensureRenderCache(mesh);
    return cache == nullptr ? 0 : copyBuffer(cache->colors, out, max_floats);
}

const float* cyber_mesh_positions_ptr(const CyberMesh* mesh, size_t* out_count) {
    const CyberRenderCache* cache = ensureRenderCache(mesh);
    return bufferView(cache == nullptr ? nullptr : &cache->positions, out_count);
}

const uint32_t* cyber_mesh_triangle_indices_ptr(const CyberMesh* mesh, size_t* out_count) {
    const CyberRenderCache* cache = ensureRenderCache(mesh);
    return bufferView(cache == nullptr ? nullptr : &cache->indices, out_count);
}

const uint32_t* cyber_mesh_edge_indices_ptr(const CyberMesh* mesh, size_t* out_count) {
    const CyberRenderCache* cache = ensureRenderCache(mesh);
    return bufferView(cache == nullptr ? nullptr : &cache->edges, out_count);
}

const float* cyber_mesh_normals_ptr(const CyberMesh* mesh, size_t* out_count) {
    const CyberRenderCache* cache = ensureRenderCache(mesh);
    return bufferView(cache == nullptr ? nullptr : &cache->normals, out_count);
}

const float* cyber_mesh_colors_ptr(const CyberMesh* mesh, size_t* out_count) {
    const CyberRenderCache* cache = ensureRenderCache(mesh);
    return bufferView(cache == nullptr ? nullptr : &cache->colors, out_count);
}

// ---- overlay render state (retopology phase 3, task 3.4) -------------------
//
// Render-cache filters only: the mesh topology and stable element ids are
// untouched, but the cache (and its pointer views) is invalidated on every
// setter, exactly like a mutating edit op.

size_t cyber_mesh_live_faces(const CyberMesh* mesh, uint32_t* out_faces,
                             size_t max_faces) {
    if (mesh == nullptr) {
        return 0;
    }
    size_t total = 0;
    for (cyber::Index i = 0; i < mesh->mesh.faceCapacity(); ++i) {
        if (!mesh->mesh.isAlive(cyber::FaceId{i})) {
            continue;
        }
        if (out_faces != nullptr && total < max_faces) {
            out_faces[total] = i;
        }
        ++total;
    }
    return total;
}

CyberStatus cyber_mesh_set_hidden_faces(CyberMesh* mesh, const uint32_t* faces,
                                        size_t face_count) {
    if (mesh == nullptr || (faces == nullptr && face_count > 0)) {
        setError("cyber_mesh_set_hidden_faces: null argument");
        return CYBER_ERR_INVALID_ARG;
    }
    try {
        std::unordered_set<std::uint32_t> hidden;
        hidden.reserve(face_count);
        for (size_t i = 0; i < face_count; ++i) {
            hidden.insert(faces[i]);
        }
        mesh->hiddenFaces = std::move(hidden);
        mesh->render = CyberRenderCache{};
        clearError();
        return CYBER_OK;
    } catch (...) {
        setError("cyber_mesh_set_hidden_faces: allocation failure");
        return CYBER_ERR_RUNTIME;
    }
}

size_t cyber_mesh_hidden_face_count(const CyberMesh* mesh) {
    return mesh == nullptr ? 0 : mesh->hiddenFaces.size();
}

CyberStatus cyber_mesh_set_tagged_edges(CyberMesh* mesh, const uint32_t* edges,
                                        size_t edge_count) {
    if (mesh == nullptr || (edges == nullptr && edge_count > 0)) {
        setError("cyber_mesh_set_tagged_edges: null argument");
        return CYBER_ERR_INVALID_ARG;
    }
    try {
        if (edge_count == 0) {
            mesh->taggedEdges.clear();
        } else {
            mesh->taggedEdges.assign(edges, edges + edge_count);
        }
        mesh->render = CyberRenderCache{};
        clearError();
        return CYBER_OK;
    } catch (...) {
        setError("cyber_mesh_set_tagged_edges: allocation failure");
        return CYBER_ERR_RUNTIME;
    }
}

const uint32_t* cyber_mesh_tagged_edge_indices_ptr(const CyberMesh* mesh,
                                                   size_t* out_count) {
    const CyberRenderCache* cache = ensureRenderCache(mesh);
    return bufferView(cache == nullptr ? nullptr : &cache->tagged, out_count);
}

// ---- spatial queries (retopology phase 3) ---------------------------------
//
// Everything here is read-only over the mesh handle: no render-cache
// invalidation is ever needed (see the LIFETIME note in cyber_capi.h).

// Opaque snapper handle: wraps the engine's BVH-backed SurfaceSnapper (its
// BVH doubles as the raycast structure via SurfaceSnapper::bvh()).
struct CyberSnapper {
    cyber::retopo::SurfaceSnapper snapper;
};

namespace {

cyber::Vec3 toVec3(const float v[3]) { return {v[0], v[1], v[2]}; }

void writeVec3(float out[3], cyber::Vec3 v) {
    if (out != nullptr) {
        out[0] = v.x;
        out[1] = v.y;
        out[2] = v.z;
    }
}

void writeId(uint32_t* out, cyber::Index id) {
    if (out != nullptr) {
        *out = id;
    }
}

}  // namespace

CyberStatus cyber_snapper_create(const CyberMesh* target, CyberSnapper** out) {
    if (target == nullptr || out == nullptr) {
        setError("cyber_snapper_create: null argument");
        return CYBER_ERR_INVALID_ARG;
    }
    *out = nullptr;
    try {
        auto handle = std::make_unique<CyberSnapper>();
        handle->snapper = cyber::retopo::SurfaceSnapper(target->mesh);
        if (handle->snapper.empty()) {
            setError("cyber_snapper_create: target mesh has no faces");
            return CYBER_ERR_EMPTY;
        }
        clearError();
        *out = handle.release();
        return CYBER_OK;
    } catch (const std::exception& e) {
        setError(std::string("cyber_snapper_create: ") + e.what());
        return CYBER_ERR_RUNTIME;
    } catch (...) {
        setError("cyber_snapper_create: unknown error");
        return CYBER_ERR_RUNTIME;
    }
}

void cyber_snapper_free(CyberSnapper* snapper) { delete snapper; }

int cyber_snapper_snap_to_surface(const CyberSnapper* snapper, const float query[3],
                                  float out_point[3], uint32_t* out_face) {
    if (snapper == nullptr || query == nullptr || snapper->snapper.empty()) {
        return 0;
    }
    const cyber::retopo::SurfaceHit hit = snapper->snapper.snapToSurface(toVec3(query));
    writeVec3(out_point, hit.point);
    writeId(out_face, hit.face.value);
    return 1;
}

int cyber_snapper_snap_to_vertex(const CyberSnapper* snapper, const float query[3],
                                 float radius, float out_point[3], uint32_t* out_vertex) {
    if (snapper == nullptr || query == nullptr) {
        return 0;
    }
    const std::optional<cyber::retopo::VertexHit> hit =
        snapper->snapper.snapToVertex(toVec3(query), radius);
    if (!hit) {
        return 0;
    }
    writeVec3(out_point, hit->point);
    writeId(out_vertex, hit->vertex.value);
    return 1;
}

int cyber_snapper_raycast(const CyberSnapper* snapper, const float origin[3],
                          const float direction[3], float max_distance,
                          float out_point[3], float* out_t, uint32_t* out_face) {
    if (snapper == nullptr || origin == nullptr || direction == nullptr) {
        return 0;
    }
    const cyber::Vec3 dir = cyber::normalized(toVec3(direction));
    if (cyber::lengthSquared(dir) == 0.0f) {
        return 0;
    }
    const std::optional<cyber::Bvh::RayHit> hit =
        snapper->snapper.bvh().raycast(toVec3(origin), dir, max_distance);
    if (!hit) {
        return 0;
    }
    writeVec3(out_point, hit->point);
    if (out_t != nullptr) {
        *out_t = hit->t;
    }
    writeId(out_face, hit->face.value);
    return 1;
}

int cyber_mesh_nearest_vertex(const CyberMesh* mesh, const float query[3],
                              float max_distance, uint32_t* out_vertex,
                              float out_position[3]) {
    if (mesh == nullptr || query == nullptr) {
        return 0;
    }
    const std::optional<cyber::retopo::VertexPick> pick =
        cyber::retopo::nearestVertex(mesh->mesh, toVec3(query), max_distance);
    if (!pick) {
        return 0;
    }
    writeId(out_vertex, pick->vertex.value);
    writeVec3(out_position, pick->position);
    return 1;
}

int cyber_mesh_nearest_vertex_excluding(const CyberMesh* mesh, const float query[3],
                                        float max_distance, uint32_t exclude_vertex,
                                        uint32_t* out_vertex, float out_position[3]) {
    if (mesh == nullptr || query == nullptr) {
        return 0;
    }
    const std::optional<cyber::retopo::VertexPick> pick = cyber::retopo::nearestVertex(
        mesh->mesh, toVec3(query), max_distance, cyber::VertexId{exclude_vertex});
    if (!pick) {
        return 0;
    }
    writeId(out_vertex, pick->vertex.value);
    writeVec3(out_position, pick->position);
    return 1;
}

int cyber_mesh_nearest_edge(const CyberMesh* mesh, const float query[3],
                            float max_distance, uint32_t* out_edge,
                            float out_point[3]) {
    if (mesh == nullptr || query == nullptr) {
        return 0;
    }
    const std::optional<cyber::retopo::EdgePick> pick =
        cyber::retopo::nearestEdge(mesh->mesh, toVec3(query), max_distance);
    if (!pick) {
        return 0;
    }
    writeId(out_edge, pick->edge.value);
    writeVec3(out_point, pick->point);
    return 1;
}

int cyber_mesh_edge_endpoints(const CyberMesh* mesh, uint32_t edge,
                              uint32_t out_vertices[2]) {
    if (mesh == nullptr || out_vertices == nullptr ||
        !mesh->mesh.isAlive(cyber::EdgeId{edge})) {
        return 0;
    }
    const auto [v0, v1] = mesh->mesh.edgeVertices(cyber::EdgeId{edge});
    out_vertices[0] = v0.value;
    out_vertices[1] = v1.value;
    return 1;
}

int cyber_mesh_is_boundary_edge(const CyberMesh* mesh, uint32_t edge) {
    if (mesh == nullptr || !mesh->mesh.isAlive(cyber::EdgeId{edge})) {
        return -1;
    }
    return mesh->mesh.isBoundaryEdge(cyber::EdgeId{edge}) ? 1 : 0;
}

int cyber_mesh_vertex_position(const CyberMesh* mesh, uint32_t vertex,
                               float out_position[3]) {
    if (mesh == nullptr || out_position == nullptr ||
        !mesh->mesh.isAlive(cyber::VertexId{vertex})) {
        return 0;
    }
    writeVec3(out_position, mesh->mesh.position(cyber::VertexId{vertex}));
    return 1;
}

int cyber_mesh_edge_faces(const CyberMesh* mesh, uint32_t edge,
                          uint32_t out_faces[2], size_t out_sizes[2]) {
    if (mesh == nullptr || !mesh->mesh.isAlive(cyber::EdgeId{edge})) {
        return -1;
    }
    const std::vector<cyber::FaceId> faces = mesh->mesh.edgeFaces(cyber::EdgeId{edge});
    const size_t n = std::min<size_t>(faces.size(), 2);
    for (size_t i = 0; i < n; ++i) {
        if (out_faces != nullptr) {
            out_faces[i] = faces[i].value;
        }
        if (out_sizes != nullptr) {
            out_sizes[i] = mesh->mesh.faceVertices(faces[i]).size();
        }
    }
    return static_cast<int>(faces.size());
}

size_t cyber_mesh_shortest_vertex_path(const CyberMesh* mesh, uint32_t from, uint32_t to,
                                       uint32_t* out_vertices, size_t max_vertices) {
    if (mesh == nullptr) {
        return 0;
    }
    const std::vector<cyber::VertexId> path = cyber::retopo::shortestVertexPath(
        mesh->mesh, cyber::VertexId{from}, cyber::VertexId{to});
    if (out_vertices != nullptr) {
        const size_t n = std::min(path.size(), max_vertices);
        for (size_t i = 0; i < n; ++i) {
            out_vertices[i] = path[i].value;
        }
    }
    return path.size();
}

size_t cyber_mesh_boundary_loop(const CyberMesh* mesh, uint32_t edge,
                                uint32_t* out_vertices, size_t max_vertices,
                                int* out_closed) {
    if (out_closed != nullptr) {
        *out_closed = 0;
    }
    if (mesh == nullptr) {
        return 0;
    }
    const cyber::retopo::BoundaryChain chain =
        cyber::retopo::boundaryChain(mesh->mesh, cyber::EdgeId{edge});
    if (out_closed != nullptr) {
        *out_closed = chain.closed ? 1 : 0;
    }
    if (out_vertices != nullptr) {
        const size_t n = std::min(chain.vertices.size(), max_vertices);
        for (size_t i = 0; i < n; ++i) {
            out_vertices[i] = chain.vertices[i].value;
        }
    }
    return chain.vertices.size();
}

// ---- quad-loop topology queries (retopology phase 3, task 3.4) -------------

namespace {

// copy_positions convention: total count returned, at most max entries
// filled.
size_t copyEdgeIds(const std::vector<cyber::EdgeId>& edges, uint32_t* out,
                   size_t max_edges) {
    if (out != nullptr) {
        const size_t n = std::min(edges.size(), max_edges);
        for (size_t i = 0; i < n; ++i) {
            out[i] = edges[i].value;
        }
    }
    return edges.size();
}

}  // namespace

size_t cyber_mesh_edge_loop(const CyberMesh* mesh, uint32_t edge, uint32_t* out_edges,
                            size_t max_edges) {
    if (mesh == nullptr) {
        return 0;
    }
    return copyEdgeIds(cyber::retopo::edgeLoopFrom(mesh->mesh, cyber::EdgeId{edge}),
                       out_edges, max_edges);
}

size_t cyber_mesh_quad_ring(const CyberMesh* mesh, uint32_t edge, uint32_t* out_edges,
                            size_t max_edges, int* out_closed) {
    if (out_closed != nullptr) {
        *out_closed = 0;
    }
    if (mesh == nullptr) {
        return 0;
    }
    const cyber::retopo::QuadRing ring =
        cyber::retopo::quadRingFromEdge(mesh->mesh, cyber::EdgeId{edge});
    if (out_closed != nullptr) {
        *out_closed = ring.closed ? 1 : 0;
    }
    return copyEdgeIds(ring.edges, out_edges, max_edges);
}

// ---- loop metrics (retopology phase 4, task 4.3) --------------------------

CyberStatus cyber_mesh_loop_metrics(const CyberMesh* mesh, uint32_t edge,
                                    const CyberSnapper* snapper,
                                    CyberLoopMetrics* out_metrics) {
    if (mesh == nullptr || out_metrics == nullptr) {
        return CYBER_ERR_INVALID_ARG;
    }
    *out_metrics = CyberLoopMetrics{};
    out_metrics->endpoint_a = CYBER_INVALID_ID;
    out_metrics->endpoint_b = CYBER_INVALID_ID;

    const cyber::retopo::EdgeLoopMetrics metrics = cyber::retopo::measureEdgeLoop(
        mesh->mesh, cyber::EdgeId{edge},
        snapper == nullptr ? nullptr : &snapper->snapper);

    out_metrics->edge_count = static_cast<uint32_t>(metrics.edgeCount);
    out_metrics->vertex_count = static_cast<uint32_t>(metrics.vertexCount);
    out_metrics->closed = metrics.closed ? 1 : 0;
    out_metrics->length = metrics.length;
    if (metrics.endpoints.has_value()) {
        out_metrics->has_endpoints = 1;
        out_metrics->endpoint_a = metrics.endpoints->first.value;
        out_metrics->endpoint_b = metrics.endpoints->second.value;
    }
    out_metrics->boundary_edge_count =
        static_cast<uint32_t>(metrics.boundaryEdgeCount);
    out_metrics->snap_measured = metrics.snapMeasured ? 1 : 0;
    out_metrics->snapped_vertex_count =
        static_cast<uint32_t>(metrics.snappedVertexCount);
    out_metrics->max_snap_distance = metrics.maxSnapDistance;
    return CYBER_OK;
}

// ---- mesh editing (retopology phase 3, task 3.3) ---------------------------
//
// Every mutating entry point here resets the handle's render cache (the
// LIFETIME contract in cyber_capi.h): pointer views die on mutation and the
// next render-data accessor rebuilds the cache lazily from the mutated mesh.

namespace {

// Drops the (possibly built) render cache after a mutation. Assigning a
// fresh value releases the old vectors, so stale geometry can never leak
// into a rebuilt cache.
void invalidateRenderCache(CyberMesh* mesh) { mesh->render = CyberRenderCache{}; }

const cyber::retopo::SurfaceSnapper* snapperOf(const CyberSnapper* snapper) {
    return snapper == nullptr ? nullptr : &snapper->snapper;
}

cyber::retopo::PinSet makePinSet(const uint32_t* pinned, size_t pinned_count) {
    cyber::retopo::PinSet pins;
    if (pinned != nullptr) {
        for (size_t i = 0; i < pinned_count; ++i) {
            pins.pin(cyber::VertexId{pinned[i]});
        }
    }
    return pins;
}

// Shared prologue/epilogue for the editing ops: argument checks, exception
// containment, and render-cache invalidation. The cache is dropped on
// success AND when an exception escapes the body: an engine op can throw
// after it already mutated topology/positions (std::bad_alloc, invariant
// throws mid-operation), and keeping the pre-mutation cache would desync
// the viewport from what payloadData() serializes until an unrelated
// reload. Error STATUS returns deliberately do not invalidate: bodies
// validate arguments before touching the mesh, so a status failure means
// the mesh (its live element set) is unchanged.
template <typename Body>
CyberStatus runMeshEdit(CyberMesh* mesh, const char* name, Body body) {
    if (mesh == nullptr) {
        setError(std::string(name) + ": null mesh");
        return CYBER_ERR_INVALID_ARG;
    }
    try {
        const CyberStatus status = body();
        if (status == CYBER_OK) {
            invalidateRenderCache(mesh);
            clearError();
        }
        return status;
    } catch (const std::exception& e) {
        // The op may have partially mutated the mesh before throwing —
        // never serve a pre-mutation cache over a mutated mesh.
        invalidateRenderCache(mesh);
        setError(std::string(name) + ": " + e.what());
        return CYBER_ERR_RUNTIME;
    } catch (...) {
        invalidateRenderCache(mesh);
        setError(std::string(name) + ": unknown error");
        return CYBER_ERR_RUNTIME;
    }
}

}  // namespace

CyberStatus cyber_retopo_create_face(CyberMesh* mesh, const float* points_xyz,
                                     size_t point_count, const CyberSnapper* snapper,
                                     uint32_t* out_face) {
    return runMeshEdit(mesh, "cyber_retopo_create_face", [&] {
        if (points_xyz == nullptr || point_count < 3 || point_count > 4) {
            setError("cyber_retopo_create_face: need 3 or 4 points");
            return CYBER_ERR_INVALID_ARG;
        }
        // Snap first, then reject degenerate rings BEFORE touching the mesh
        // (two distinct screen corners can land on one Target point under a
        // steep projection), so a failure leaves the handle untouched.
        std::vector<cyber::Vec3> points;
        points.reserve(point_count);
        const cyber::retopo::SurfaceSnapper* snap = snapperOf(snapper);
        for (size_t i = 0; i < point_count; ++i) {
            const cyber::Vec3 p = toVec3(points_xyz + i * 3);
            points.push_back(snap != nullptr && !snap->empty() ? snap->snapToSurface(p).point
                                                               : p);
        }
        for (size_t i = 0; i < points.size(); ++i) {
            for (size_t j = i + 1; j < points.size(); ++j) {
                if (cyber::lengthSquared(points[i] - points[j]) < 1e-12f) {
                    setError("cyber_retopo_create_face: degenerate polygon");
                    return CYBER_ERR_INVALID_ARG;
                }
            }
        }
        // Vertices are already snapped; track their ids so an addFace
        // rejection (paranoia at this point) can undo the additions exactly.
        std::vector<cyber::VertexId> verts;
        verts.reserve(points.size());
        for (const cyber::Vec3& p : points) {
            verts.push_back(mesh->mesh.addVertex(p));
        }
        const cyber::FaceId face = mesh->mesh.addFace(verts);
        if (!face.valid()) {
            for (const cyber::VertexId v : verts) {
                if (mesh->mesh.isAlive(v) && mesh->mesh.vertexEdges(v).empty()) {
                    mesh->mesh.removeIsolatedVertex(v);
                }
            }
            setError("cyber_retopo_create_face: degenerate polygon");
            return CYBER_ERR_INVALID_ARG;
        }
        writeId(out_face, face.value);
        return CYBER_OK;
    });
}

CyberStatus cyber_retopo_tweak_vertex(CyberMesh* mesh, uint32_t vertex,
                                      const float target[3], const CyberSnapper* snapper) {
    return runMeshEdit(mesh, "cyber_retopo_tweak_vertex", [&] {
        if (target == nullptr || !mesh->mesh.isAlive(cyber::VertexId{vertex})) {
            setError("cyber_retopo_tweak_vertex: dead vertex or null target");
            return CYBER_ERR_INVALID_ARG;
        }
        cyber::retopo::tweakVertex(mesh->mesh, cyber::VertexId{vertex}, toVec3(target),
                                   snapperOf(snapper));
        return CYBER_OK;
    });
}

CyberStatus cyber_retopo_move(CyberMesh* mesh, uint32_t seed_vertex,
                              const float displacement[3], float radius,
                              const uint32_t* pinned, size_t pinned_count,
                              const CyberSnapper* snapper) {
    return runMeshEdit(mesh, "cyber_retopo_move", [&] {
        if (displacement == nullptr || !mesh->mesh.isAlive(cyber::VertexId{seed_vertex})) {
            setError("cyber_retopo_move: dead seed vertex or null displacement");
            return CYBER_ERR_INVALID_ARG;
        }
        if (!(radius > 0.0f)) {
            setError("cyber_retopo_move: radius must be positive");
            return CYBER_ERR_INVALID_PARAM;
        }
        cyber::retopo::MoveParams params;
        params.seed = cyber::VertexId{seed_vertex};
        params.displacement = toVec3(displacement);
        params.radius = radius;
        const cyber::retopo::PinSet pins = makePinSet(pinned, pinned_count);
        cyber::retopo::move(mesh->mesh, params, &pins, snapperOf(snapper));
        return CYBER_OK;
    });
}

CyberStatus cyber_retopo_relax(CyberMesh* mesh, const float center[3], float radius,
                               float strength, int iterations, int auto_pin_corners,
                               const uint32_t* pinned, size_t pinned_count,
                               const CyberSnapper* snapper) {
    return runMeshEdit(mesh, "cyber_retopo_relax", [&] {
        if (center == nullptr) {
            setError("cyber_retopo_relax: null center");
            return CYBER_ERR_INVALID_ARG;
        }
        if (!(strength >= 0.0f && strength <= 1.0f) || iterations < 1) {
            setError("cyber_retopo_relax: strength must be in [0,1], iterations >= 1");
            return CYBER_ERR_INVALID_PARAM;
        }
        cyber::retopo::RelaxParams params;
        params.strength = strength;
        params.iterations = iterations;
        params.brushCenter = toVec3(center);
        params.brushRadius = radius;
        params.autoPinCorners = auto_pin_corners != 0;
        const cyber::retopo::PinSet pins = makePinSet(pinned, pinned_count);
        cyber::retopo::relax(mesh->mesh, params, &pins, snapperOf(snapper));
        return CYBER_OK;
    });
}

CyberStatus cyber_retopo_erase(CyberMesh* mesh, const float center[3], float base_radius,
                               float pressure, size_t* out_removed) {
    return runMeshEdit(mesh, "cyber_retopo_erase", [&] {
        if (center == nullptr) {
            setError("cyber_retopo_erase: null center");
            return CYBER_ERR_INVALID_ARG;
        }
        if (!(base_radius > 0.0f)) {
            setError("cyber_retopo_erase: base_radius must be positive");
            return CYBER_ERR_INVALID_PARAM;
        }
        const size_t removed =
            cyber::retopo::erase(mesh->mesh, toVec3(center), base_radius, pressure);
        if (out_removed != nullptr) {
            *out_removed = removed;
        }
        return CYBER_OK;
    });
}

CyberStatus cyber_retopo_delete_faces(CyberMesh* mesh, const uint32_t* faces,
                                      size_t face_count, size_t* out_removed) {
    return runMeshEdit(mesh, "cyber_retopo_delete_faces", [&] {
        if (faces == nullptr && face_count > 0) {
            setError("cyber_retopo_delete_faces: null face list");
            return CYBER_ERR_INVALID_ARG;
        }
        std::vector<cyber::FaceId> doomed;
        std::vector<cyber::VertexId> touched;
        doomed.reserve(face_count);
        for (size_t i = 0; i < face_count; ++i) {
            const cyber::FaceId f{faces[i]};
            if (mesh->mesh.isAlive(f)) {
                doomed.push_back(f);
                for (const cyber::VertexId v : mesh->mesh.faceVertices(f)) {
                    touched.push_back(v);
                }
            }
        }
        cyber::retopo::deleteFaces(mesh->mesh, doomed);
        for (const cyber::VertexId v : touched) {
            if (mesh->mesh.isAlive(v) && mesh->mesh.vertexEdges(v).empty()) {
                mesh->mesh.removeIsolatedVertex(v);
            }
        }
        if (out_removed != nullptr) {
            *out_removed = doomed.size();
        }
        return CYBER_OK;
    });
}

CyberStatus cyber_retopo_insert_loop(CyberMesh* mesh, uint32_t edge, float t,
                                     size_t* out_new_faces) {
    return runMeshEdit(mesh, "cyber_retopo_insert_loop", [&] {
        if (!mesh->mesh.isAlive(cyber::EdgeId{edge})) {
            setError("cyber_retopo_insert_loop: dead edge");
            return CYBER_ERR_INVALID_ARG;
        }
        if (!(t > 0.0f && t < 1.0f)) {
            setError("cyber_retopo_insert_loop: t must be in (0, 1)");
            return CYBER_ERR_INVALID_PARAM;
        }
        const cyber::retopo::LoopInsertResult result =
            cyber::retopo::insertLoopAcrossRing(mesh->mesh, cyber::EdgeId{edge}, t);
        if (result.newFaces.empty()) {
            setError("cyber_retopo_insert_loop: edge borders no quad");
            return CYBER_ERR_INVALID_ARG;
        }
        if (out_new_faces != nullptr) {
            *out_new_faces = result.newFaces.size();
        }
        return CYBER_OK;
    });
}

CyberStatus cyber_retopo_create_grid(CyberMesh* mesh, const float* points_xyz,
                                     size_t rows, size_t cols,
                                     const CyberSnapper* snapper, size_t* out_faces) {
    return runMeshEdit(mesh, "cyber_retopo_create_grid", [&] {
        if (points_xyz == nullptr || rows < 1 || cols < 1) {
            setError("cyber_retopo_create_grid: null lattice or empty grid");
            return CYBER_ERR_INVALID_ARG;
        }
        const size_t stride = cols + 1;
        const size_t pointCount = (rows + 1) * stride;
        // Snap first and validate BEFORE touching the mesh, exactly like
        // create_face: a degenerate lattice leaves the handle untouched.
        std::vector<cyber::Vec3> points;
        points.reserve(pointCount);
        const cyber::retopo::SurfaceSnapper* snap = snapperOf(snapper);
        for (size_t i = 0; i < pointCount; ++i) {
            const cyber::Vec3 p = toVec3(points_xyz + i * 3);
            points.push_back(snap != nullptr && !snap->empty() ? snap->snapToSurface(p).point
                                                               : p);
        }
        for (size_t i = 0; i < points.size(); ++i) {
            for (size_t j = i + 1; j < points.size(); ++j) {
                if (cyber::lengthSquared(points[i] - points[j]) < 1e-12f) {
                    setError("cyber_retopo_create_grid: degenerate lattice");
                    return CYBER_ERR_INVALID_ARG;
                }
            }
        }
        std::vector<cyber::VertexId> verts;
        verts.reserve(points.size());
        for (const cyber::Vec3& p : points) {
            verts.push_back(mesh->mesh.addVertex(p));
        }
        std::vector<cyber::FaceId> faces;
        faces.reserve(rows * cols);
        bool failed = false;
        for (size_t r = 0; r < rows && !failed; ++r) {
            for (size_t c = 0; c < cols && !failed; ++c) {
                const size_t corner = r * stride + c;
                const std::array<cyber::VertexId, 4> ring = {
                    verts[corner], verts[corner + 1], verts[corner + stride + 1],
                    verts[corner + stride]};
                const cyber::FaceId f = mesh->mesh.addFace(ring);
                if (f.valid()) {
                    faces.push_back(f);
                } else {
                    failed = true;
                }
            }
        }
        if (failed) {
            // Roll the partial block back so the stroke stays atomic.
            for (const cyber::FaceId f : faces) {
                mesh->mesh.removeFace(f);
            }
            for (const cyber::VertexId v : verts) {
                if (mesh->mesh.isAlive(v) && mesh->mesh.vertexEdges(v).empty()) {
                    mesh->mesh.removeIsolatedVertex(v);
                }
            }
            setError("cyber_retopo_create_grid: degenerate cell");
            return CYBER_ERR_INVALID_ARG;
        }
        if (out_faces != nullptr) {
            *out_faces = faces.size();
        }
        return CYBER_OK;
    });
}

CyberStatus cyber_retopo_dissolve_edges(CyberMesh* mesh, const uint32_t* edges,
                                        size_t edge_count, size_t* out_dissolved) {
    return runMeshEdit(mesh, "cyber_retopo_dissolve_edges", [&] {
        if (edges == nullptr && edge_count > 0) {
            setError("cyber_retopo_dissolve_edges: null edge list");
            return CYBER_ERR_INVALID_ARG;
        }
        size_t dissolved = 0;
        for (size_t i = 0; i < edge_count; ++i) {
            // dissolveEdge revalidates per edge: ids invalidated by an
            // earlier merge in the same batch are skipped, never crash.
            if (cyber::retopo::dissolveEdge(mesh->mesh, cyber::EdgeId{edges[i]}).valid()) {
                ++dissolved;
            }
        }
        if (out_dissolved != nullptr) {
            *out_dissolved = dissolved;
        }
        return CYBER_OK;
    });
}

CyberStatus cyber_retopo_merge_vertices(CyberMesh* mesh, uint32_t keep, uint32_t remove,
                                        int at_midpoint) {
    return runMeshEdit(mesh, "cyber_retopo_merge_vertices", [&] {
        const cyber::VertexId keepId{keep};
        const cyber::VertexId removeId{remove};
        if (!mesh->mesh.isAlive(keepId) || !mesh->mesh.isAlive(removeId) ||
            keep == remove) {
            setError("cyber_retopo_merge_vertices: dead or identical vertices");
            return CYBER_ERR_INVALID_ARG;
        }
        const cyber::Vec3 midpoint =
            (mesh->mesh.position(keepId) + mesh->mesh.position(removeId)) * 0.5f;
        if (!mesh->mesh.mergeVertices(keepId, removeId)) {
            setError("cyber_retopo_merge_vertices: merge rejected");
            return CYBER_ERR_INVALID_ARG;
        }
        if (at_midpoint != 0 && mesh->mesh.isAlive(keepId)) {
            mesh->mesh.setPosition(keepId, midpoint);
        }
        return CYBER_OK;
    });
}

CyberStatus cyber_retopo_rotate_edge(CyberMesh* mesh, uint32_t edge) {
    return runMeshEdit(mesh, "cyber_retopo_rotate_edge", [&] {
        if (!cyber::retopo::rotateEdgeAny(mesh->mesh, cyber::EdgeId{edge})) {
            setError("cyber_retopo_rotate_edge: edge cannot rotate "
                     "(dead, boundary, non-tri/quad pair, or fold-over)");
            return CYBER_ERR_INVALID_ARG;
        }
        return CYBER_OK;
    });
}

// ---- build tools (retopology phase 4, task 4.1) -----------------------------

CyberStatus cyber_retopo_build_face(CyberMesh* mesh, size_t count,
                                    const uint32_t* vertex_ids, const float* points_xyz,
                                    const CyberSnapper* snapper, uint32_t* out_face,
                                    uint32_t* out_ring_vertices) {
    return runMeshEdit(mesh, "cyber_retopo_build_face", [&] {
        if (vertex_ids == nullptr || count < 3 || count > 4) {
            setError("cyber_retopo_build_face: need 3 or 4 ring slots");
            return CYBER_ERR_INVALID_ARG;
        }
        // Resolve the final ring POSITIONS first (existing verts + snapped
        // new points) and reject every degenerate case BEFORE touching the
        // mesh, so a failure leaves the handle untouched.
        const cyber::retopo::SurfaceSnapper* snap = snapperOf(snapper);
        std::vector<cyber::Vec3> positions;
        positions.reserve(count);
        for (size_t i = 0; i < count; ++i) {
            if (vertex_ids[i] == CYBER_BUILD_NEW_VERTEX) {
                if (points_xyz == nullptr) {
                    setError("cyber_retopo_build_face: new-vertex slot without points");
                    return CYBER_ERR_INVALID_ARG;
                }
                const cyber::Vec3 p = toVec3(points_xyz + i * 3);
                positions.push_back(
                    snap != nullptr && !snap->empty() ? snap->snapToSurface(p).point : p);
            } else {
                const cyber::VertexId v{vertex_ids[i]};
                if (!mesh->mesh.isAlive(v)) {
                    setError("cyber_retopo_build_face: dead ring vertex");
                    return CYBER_ERR_INVALID_ARG;
                }
                positions.push_back(mesh->mesh.position(v));
            }
        }
        for (size_t i = 0; i < count; ++i) {
            for (size_t j = i + 1; j < count; ++j) {
                const bool both_existing = vertex_ids[i] != CYBER_BUILD_NEW_VERTEX &&
                                           vertex_ids[j] != CYBER_BUILD_NEW_VERTEX;
                if (both_existing && vertex_ids[i] == vertex_ids[j]) {
                    setError("cyber_retopo_build_face: repeated ring vertex");
                    return CYBER_ERR_INVALID_ARG;
                }
                if (cyber::lengthSquared(positions[i] - positions[j]) < 1e-12f) {
                    setError("cyber_retopo_build_face: degenerate polygon");
                    return CYBER_ERR_INVALID_ARG;
                }
            }
        }
        // Materialize the ring; on an addFace rejection remove exactly the
        // vertices this call created (existing topology stays untouched).
        std::vector<cyber::VertexId> ring;
        std::vector<cyber::VertexId> created;
        ring.reserve(count);
        for (size_t i = 0; i < count; ++i) {
            if (vertex_ids[i] == CYBER_BUILD_NEW_VERTEX) {
                const cyber::VertexId nv = mesh->mesh.addVertex(positions[i]);
                ring.push_back(nv);
                created.push_back(nv);
            } else {
                ring.push_back(cyber::VertexId{vertex_ids[i]});
            }
        }
        // Consistent winding: when two consecutive ring slots reuse an
        // existing BOUNDARY edge, wind the new face OPPOSITE to that
        // edge's face (addFace accepts either winding; shading needs
        // coherent normals across the weld).
        bool flip = false;
        for (size_t i = 0; i < count && !flip; ++i) {
            const cyber::VertexId u = ring[i];
            const cyber::VertexId v = ring[(i + 1) % count];
            const cyber::EdgeId shared = mesh->mesh.edgeBetween(u, v);
            if (!shared.valid()) {
                continue;
            }
            const std::vector<cyber::FaceId> adjacent = mesh->mesh.edgeFaces(shared);
            if (adjacent.size() != 1) {
                continue;
            }
            const std::vector<cyber::VertexId> fv = mesh->mesh.faceVertices(adjacent.front());
            for (size_t k = 0; k < fv.size(); ++k) {
                if (fv[k] == u && fv[(k + 1) % fv.size()] == v) {
                    flip = true;
                    break;
                }
            }
        }
        if (flip) {
            std::reverse(ring.begin(), ring.end());
        }
        const cyber::FaceId face = mesh->mesh.addFace(ring);
        if (!face.valid()) {
            for (const cyber::VertexId v : created) {
                if (mesh->mesh.isAlive(v) && mesh->mesh.vertexEdges(v).empty()) {
                    mesh->mesh.removeIsolatedVertex(v);
                }
            }
            setError("cyber_retopo_build_face: face rejected");
            return CYBER_ERR_INVALID_ARG;
        }
        writeId(out_face, face.value);
        if (out_ring_vertices != nullptr) {
            for (size_t i = 0; i < count; ++i) {
                out_ring_vertices[i] = ring[i].value;
            }
        }
        return CYBER_OK;
    });
}

CyberStatus cyber_retopo_grow_boundary_edge(CyberMesh* mesh, uint32_t edge,
                                            const float point_xyz[3],
                                            const CyberSnapper* snapper,
                                            uint32_t* out_vertex) {
    return runMeshEdit(mesh, "cyber_retopo_grow_boundary_edge", [&] {
        const cyber::EdgeId e{edge};
        if (point_xyz == nullptr || !mesh->mesh.isAlive(e)) {
            setError("cyber_retopo_grow_boundary_edge: null point or dead edge");
            return CYBER_ERR_INVALID_ARG;
        }
        const std::vector<cyber::FaceId> faces = mesh->mesh.edgeFaces(e);
        if (faces.size() != 1) {
            setError("cyber_retopo_grow_boundary_edge: edge is not a boundary edge");
            return CYBER_ERR_INVALID_ARG;
        }
        if (mesh->mesh.faceVertices(faces.front()).size() != 3) {
            setError("cyber_retopo_grow_boundary_edge: face is not a triangle");
            return CYBER_ERR_INVALID_ARG;
        }
        const cyber::retopo::SurfaceSnapper* snap = snapperOf(snapper);
        const cyber::Vec3 p = toVec3(point_xyz);
        const cyber::Vec3 target =
            snap != nullptr && !snap->empty() ? snap->snapToSurface(p).point : p;
        const cyber::VertexId nv = mesh->mesh.splitEdge(e, 0.5f);
        if (!nv.valid()) {
            setError("cyber_retopo_grow_boundary_edge: split rejected");
            return CYBER_ERR_RUNTIME;
        }
        mesh->mesh.setPosition(nv, target);
        writeId(out_vertex, nv.value);
        return CYBER_OK;
    });
}

CyberStatus cyber_retopo_distribute_path(CyberMesh* mesh, const uint32_t* vertices,
                                         size_t count, const CyberSnapper* snapper) {
    return runMeshEdit(mesh, "cyber_retopo_distribute_path", [&] {
        if (vertices == nullptr || count < 3) {
            setError("cyber_retopo_distribute_path: need an ordered chain of >= 3 vertices");
            return CYBER_ERR_INVALID_ARG;
        }
        // Validate the whole chain BEFORE moving anything: alive, distinct,
        // and consecutive entries joined by live edges.
        std::unordered_set<uint32_t> seen;
        for (size_t i = 0; i < count; ++i) {
            if (!mesh->mesh.isAlive(cyber::VertexId{vertices[i]})) {
                setError("cyber_retopo_distribute_path: dead chain vertex");
                return CYBER_ERR_INVALID_ARG;
            }
            if (!seen.insert(vertices[i]).second) {
                setError("cyber_retopo_distribute_path: repeated chain vertex");
                return CYBER_ERR_INVALID_ARG;
            }
            if (i > 0 && !mesh->mesh
                              .edgeBetween(cyber::VertexId{vertices[i - 1]},
                                           cyber::VertexId{vertices[i]})
                              .valid()) {
                setError("cyber_retopo_distribute_path: chain break (no edge between "
                         "consecutive vertices)");
                return CYBER_ERR_INVALID_ARG;
            }
        }
        std::vector<cyber::Vec3> polyline;
        polyline.reserve(count);
        for (size_t i = 0; i < count; ++i) {
            polyline.push_back(mesh->mesh.position(cyber::VertexId{vertices[i]}));
        }
        const std::vector<cyber::Vec3> even =
            cyber::retopo::pathDistribute(polyline, static_cast<int>(count));
        if (even.size() != count) {
            setError("cyber_retopo_distribute_path: degenerate chain polyline");
            return CYBER_ERR_INVALID_ARG;
        }
        // Endpoints stay fixed; interior vertices move (snapped when given).
        const cyber::retopo::SurfaceSnapper* snap = snapperOf(snapper);
        for (size_t i = 1; i + 1 < count; ++i) {
            const cyber::Vec3 p =
                snap != nullptr && !snap->empty() ? snap->snapToSurface(even[i]).point : even[i];
            mesh->mesh.setPosition(cyber::VertexId{vertices[i]}, p);
        }
        return CYBER_OK;
    });
}

CyberStatus cyber_retopo_surface_cut(CyberMesh* mesh, const float a[3], const float b[3],
                                     const float view_dir[3], int triangulate_ngons,
                                     const CyberSnapper* snapper, size_t* out_split_edges,
                                     size_t* out_split_faces) {
    return runMeshEdit(mesh, "cyber_retopo_surface_cut", [&] {
        if (a == nullptr || b == nullptr || view_dir == nullptr) {
            setError("cyber_retopo_surface_cut: null segment or view direction");
            return CYBER_ERR_INVALID_ARG;
        }
        const cyber::Vec3 pa = toVec3(a);
        const cyber::Vec3 pb = toVec3(b);
        const cyber::Vec3 view = toVec3(view_dir);
        if (cyber::lengthSquared(pb - pa) < 1e-12f ||
            cyber::lengthSquared(cyber::cross(pb - pa, view)) < 1e-20f) {
            setError("cyber_retopo_surface_cut: degenerate knife segment");
            return CYBER_ERR_INVALID_ARG;
        }
        const cyber::retopo::SurfaceCutResult result = cyber::retopo::surfaceCutSegment(
            mesh->mesh, pa, pb, view, triangulate_ngons != 0, snapperOf(snapper));
        if (out_split_edges != nullptr) {
            *out_split_edges = result.splitEdges;
        }
        if (out_split_faces != nullptr) {
            *out_split_faces = result.splitFaces;
        }
        return CYBER_OK;
    });
}

// ---- camera-as-manipulator placement ops (task 4.2) ------------------------

namespace {

// 12-float column-major affine (see cyber_capi.h) -> engine Affine.
cyber::retopo::Affine toAffine(const float xf[12]) {
    cyber::retopo::Affine a;
    a.col0 = {xf[0], xf[1], xf[2]};
    a.col1 = {xf[3], xf[4], xf[5]};
    a.col2 = {xf[6], xf[7], xf[8]};
    a.translation = {xf[9], xf[10], xf[11]};
    return a;
}

// Shared chain validation for the Extend Boundary ops: alive, distinct,
// consecutive vertices joined by live edges (and the wrap edge when
// closed). Writes the typed chain to `out` and returns CYBER_OK, or sets
// the error and returns a failure status with `out` untouched.
CyberStatus validateChain(const CyberMesh* mesh, const char* name, const uint32_t* chain,
                          size_t count, int closed, std::vector<cyber::VertexId>& out) {
    if (chain == nullptr || count < 2) {
        setError(std::string(name) + ": need an ordered chain of >= 2 vertices");
        return CYBER_ERR_INVALID_ARG;
    }
    std::unordered_set<uint32_t> seen;
    for (size_t i = 0; i < count; ++i) {
        if (!mesh->mesh.isAlive(cyber::VertexId{chain[i]})) {
            setError(std::string(name) + ": dead chain vertex");
            return CYBER_ERR_INVALID_ARG;
        }
        if (!seen.insert(chain[i]).second) {
            setError(std::string(name) + ": repeated chain vertex");
            return CYBER_ERR_INVALID_ARG;
        }
        if (i > 0 && !mesh->mesh
                          .edgeBetween(cyber::VertexId{chain[i - 1]},
                                       cyber::VertexId{chain[i]})
                          .valid()) {
            setError(std::string(name) + ": chain break (no edge between consecutive "
                                         "vertices)");
            return CYBER_ERR_INVALID_ARG;
        }
    }
    if (closed != 0) {
        if (count < 3 || !mesh->mesh
                              .edgeBetween(cyber::VertexId{chain[count - 1]},
                                           cyber::VertexId{chain[0]})
                              .valid()) {
            setError(std::string(name) + ": closed chain needs >= 3 vertices and a live "
                                         "wrap edge");
            return CYBER_ERR_INVALID_ARG;
        }
    }
    out.reserve(count);
    for (size_t i = 0; i < count; ++i) {
        out.push_back(cyber::VertexId{chain[i]});
    }
    return CYBER_OK;
}

}  // namespace

CyberStatus cyber_retopo_patch_clone(CyberMesh* mesh, const uint32_t* faces,
                                     size_t face_count, const float xf[12], int flip,
                                     const CyberSnapper* snapper, uint32_t* out_new_faces,
                                     size_t* out_new_face_count) {
    return runMeshEdit(mesh, "cyber_retopo_patch_clone", [&] {
        if (faces == nullptr || face_count == 0 || xf == nullptr) {
            setError("cyber_retopo_patch_clone: null/empty faces or transform");
            return CYBER_ERR_INVALID_ARG;
        }
        std::vector<cyber::FaceId> ids;
        ids.reserve(face_count);
        std::unordered_set<uint32_t> seen;
        for (size_t i = 0; i < face_count; ++i) {
            if (!mesh->mesh.isAlive(cyber::FaceId{faces[i]})) {
                setError("cyber_retopo_patch_clone: dead face id");
                return CYBER_ERR_INVALID_ARG;
            }
            if (!seen.insert(faces[i]).second) {
                setError("cyber_retopo_patch_clone: repeated face id");
                return CYBER_ERR_INVALID_ARG;
            }
            ids.push_back(cyber::FaceId{faces[i]});
        }
        const std::vector<cyber::FaceId> cloned = cyber::retopo::patchClone(
            mesh->mesh, ids, toAffine(xf), snapperOf(snapper), flip != 0);
        if (out_new_faces != nullptr) {
            for (size_t i = 0; i < cloned.size() && i < face_count; ++i) {
                out_new_faces[i] = cloned[i].value;
            }
        }
        if (out_new_face_count != nullptr) {
            *out_new_face_count = cloned.size();
        }
        return CYBER_OK;
    });
}

CyberStatus cyber_retopo_extend_boundary_grid(CyberMesh* mesh, const uint32_t* chain,
                                              size_t count, int closed,
                                              const float offset[3], int rings,
                                              const CyberSnapper* snapper,
                                              uint32_t* out_outer_chain,
                                              size_t* out_new_faces) {
    return runMeshEdit(mesh, "cyber_retopo_extend_boundary_grid", [&] {
        std::vector<cyber::VertexId> verts;
        const CyberStatus chainStatus = validateChain(
            mesh, "cyber_retopo_extend_boundary_grid", chain, count, closed, verts);
        if (chainStatus != CYBER_OK) {
            return chainStatus;
        }
        if (offset == nullptr || cyber::lengthSquared(toVec3(offset)) <= 0.0f) {
            setError("cyber_retopo_extend_boundary_grid: zero offset");
            return CYBER_ERR_INVALID_ARG;
        }
        if (rings < 1) {
            setError("cyber_retopo_extend_boundary_grid: rings must be >= 1");
            return CYBER_ERR_INVALID_PARAM;
        }
        const cyber::retopo::BoundaryExtension extension =
            cyber::retopo::extendBoundaryRings(mesh->mesh, verts, closed != 0,
                                               toVec3(offset), rings, snapperOf(snapper));
        if (out_outer_chain != nullptr) {
            for (size_t i = 0; i < extension.outerChain.size() && i < count; ++i) {
                out_outer_chain[i] = extension.outerChain[i].value;
            }
        }
        if (out_new_faces != nullptr) {
            *out_new_faces = extension.faces.size();
        }
        return CYBER_OK;
    });
}

CyberStatus cyber_retopo_extend_boundary_fan(CyberMesh* mesh, const uint32_t* chain,
                                             size_t count, int closed,
                                             const float apex_offset[3],
                                             const CyberSnapper* snapper,
                                             uint32_t* out_apex, size_t* out_new_faces) {
    return runMeshEdit(mesh, "cyber_retopo_extend_boundary_fan", [&] {
        std::vector<cyber::VertexId> verts;
        const CyberStatus chainStatus = validateChain(
            mesh, "cyber_retopo_extend_boundary_fan", chain, count, closed, verts);
        if (chainStatus != CYBER_OK) {
            return chainStatus;
        }
        if (apex_offset == nullptr) {
            setError("cyber_retopo_extend_boundary_fan: null apex offset");
            return CYBER_ERR_INVALID_ARG;
        }
        // The engine fan emits {chain[i], chain[i+1], apex}: reverse the
        // chain when the existing face already traverses it forward, and
        // append the wrap pair for closed chains (ids, not clones).
        if (cyber::retopo::detail::faceTraverses(mesh->mesh, verts[0], verts[1])) {
            std::reverse(verts.begin(), verts.end());
        }
        if (closed != 0) {
            verts.push_back(verts.front());
        }
        const std::vector<cyber::FaceId> faces = cyber::retopo::extendBoundaryFan(
            mesh->mesh, verts, toVec3(apex_offset), snapperOf(snapper));
        if (faces.empty()) {
            setError("cyber_retopo_extend_boundary_fan: degenerate fan");
            return CYBER_ERR_INVALID_ARG;
        }
        if (out_apex != nullptr) {
            // The apex is the fan's shared third corner.
            const std::vector<cyber::VertexId> ring = mesh->mesh.faceVertices(faces.front());
            if (ring.size() == 3) {
                *out_apex = ring[2].value;
            }
        }
        if (out_new_faces != nullptr) {
            *out_new_faces = faces.size();
        }
        return CYBER_OK;
    });
}

CyberStatus cyber_retopo_draw_strip(CyberMesh* mesh, const float* path_xyz,
                                    size_t point_count, float width,
                                    const float view_dir[3], uint32_t start_a,
                                    uint32_t start_b, const CyberSnapper* snapper,
                                    size_t* out_new_faces) {
    return runMeshEdit(mesh, "cyber_retopo_draw_strip", [&] {
        if (path_xyz == nullptr || point_count == 0 || view_dir == nullptr) {
            setError("cyber_retopo_draw_strip: null/empty path or view direction");
            return CYBER_ERR_INVALID_ARG;
        }
        if (!(width > 0.0f)) {
            setError("cyber_retopo_draw_strip: width must be positive");
            return CYBER_ERR_INVALID_PARAM;
        }
        if (cyber::lengthSquared(toVec3(view_dir)) <= 0.0f) {
            setError("cyber_retopo_draw_strip: degenerate view direction");
            return CYBER_ERR_INVALID_ARG;
        }
        const cyber::VertexId a{start_a};
        const cyber::VertexId b{start_b};
        if (!mesh->mesh.isAlive(a) || !mesh->mesh.isAlive(b) || start_a == start_b) {
            setError("cyber_retopo_draw_strip: dead or identical start vertices");
            return CYBER_ERR_INVALID_ARG;
        }
        const cyber::EdgeId startEdge = mesh->mesh.edgeBetween(a, b);
        if (!startEdge.valid() || mesh->mesh.edgeFaces(startEdge).size() != 1) {
            setError("cyber_retopo_draw_strip: start vertices must span a boundary edge");
            return CYBER_ERR_INVALID_ARG;
        }
        std::vector<cyber::Vec3> path;
        path.reserve(point_count);
        for (size_t i = 0; i < point_count; ++i) {
            path.push_back(toVec3(path_xyz + i * 3));
        }
        const cyber::retopo::StripResult strip = cyber::retopo::drawStripPath(
            mesh->mesh, path, width, toVec3(view_dir), a, b, snapperOf(snapper));
        if (strip.faces.empty()) {
            setError("cyber_retopo_draw_strip: degenerate strip");
            return CYBER_ERR_INVALID_ARG;
        }
        if (out_new_faces != nullptr) {
            *out_new_faces = strip.faces.size();
        }
        return CYBER_OK;
    });
}

CyberStatus cyber_retopo_transform_vertices(CyberMesh* mesh, const uint32_t* vertices,
                                            size_t count, const float xf[12],
                                            const CyberSnapper* snapper,
                                            float resnap_epsilon, size_t* out_resnapped,
                                            float* out_max_distance) {
    return runMeshEdit(mesh, "cyber_retopo_transform_vertices", [&] {
        if (vertices == nullptr || count == 0 || xf == nullptr) {
            setError("cyber_retopo_transform_vertices: null/empty vertices or transform");
            return CYBER_ERR_INVALID_ARG;
        }
        std::vector<cyber::VertexId> ids;
        ids.reserve(count);
        std::unordered_set<uint32_t> seen;
        for (size_t i = 0; i < count; ++i) {
            if (!mesh->mesh.isAlive(cyber::VertexId{vertices[i]})) {
                setError("cyber_retopo_transform_vertices: dead vertex id");
                return CYBER_ERR_INVALID_ARG;
            }
            if (!seen.insert(vertices[i]).second) {
                setError("cyber_retopo_transform_vertices: repeated vertex id");
                return CYBER_ERR_INVALID_ARG;
            }
            ids.push_back(cyber::VertexId{vertices[i]});
        }
        cyber::retopo::transformVertices(mesh->mesh, ids, toAffine(xf));
        size_t resnapped = 0;
        float maxDistance = 0.0f;
        const cyber::retopo::SurfaceSnapper* snap = snapperOf(snapper);
        if (snap != nullptr && !snap->empty()) {
            const float eps = resnap_epsilon >= 0.0f ? resnap_epsilon : 0.0f;
            for (const cyber::VertexId v : ids) {
                const cyber::Vec3 p = mesh->mesh.position(v);
                const cyber::Vec3 q = snap->snapToSurface(p).point;
                const float moved = cyber::length(q - p);
                if (moved > eps) {
                    ++resnapped;
                    maxDistance = std::max(maxDistance, moved);
                }
                mesh->mesh.setPosition(v, q);
            }
        }
        if (out_resnapped != nullptr) {
            *out_resnapped = resnapped;
        }
        if (out_max_distance != nullptr) {
            *out_max_distance = maxDistance;
        }
        return CYBER_OK;
    });
}

// ---- symmetry (retopology phase 4, task 4.4) -------------------------------

namespace {

// Translates the capi plane description into the engine's Symmetry, or
// reports the degenerate-normal rejection. Validation happens BEFORE any
// mutation so a failure leaves the mesh untouched (the runMeshEdit contract).
bool toSymmetry(const CyberSymmetry* in, cyber::retopo::Symmetry& out) {
    if (in == nullptr) {
        return false;
    }
    const cyber::Vec3 normal{in->normal[0], in->normal[1], in->normal[2]};
    const float len = cyber::length(normal);
    if (!(len > 0.0f)) {
        return false;
    }
    out.plane.point = cyber::Vec3{in->origin[0], in->origin[1], in->origin[2]};
    out.plane.normal = normal * (1.0f / len);
    out.weldTolerance = in->weld_tolerance > 0.0f ? in->weld_tolerance : 0.0f;
    out.workingSidePositive = in->working_side_positive != 0;
    return true;
}

}  // namespace

CyberStatus cyber_retopo_snap_symmetry_plane(CyberMesh* mesh, const CyberSymmetry* symmetry,
                                             size_t* out_snapped) {
    return runMeshEdit(mesh, "cyber_retopo_snap_symmetry_plane", [&] {
        cyber::retopo::Symmetry sym;
        if (!toSymmetry(symmetry, sym)) {
            setError("cyber_retopo_snap_symmetry_plane: null or degenerate plane");
            return CYBER_ERR_INVALID_ARG;
        }
        const size_t snapped = cyber::retopo::snapNearPlane(mesh->mesh, sym);
        if (out_snapped != nullptr) {
            *out_snapped = snapped;
        }
        return CYBER_OK;
    });
}

CyberStatus cyber_retopo_apply_symmetry(CyberMesh* mesh, const CyberSymmetry* symmetry,
                                        const CyberSnapper* snapper,
                                        size_t* out_added_faces) {
    return runMeshEdit(mesh, "cyber_retopo_apply_symmetry", [&] {
        cyber::retopo::Symmetry sym;
        if (!toSymmetry(symmetry, sym)) {
            setError("cyber_retopo_apply_symmetry: null or degenerate plane");
            return CYBER_ERR_INVALID_ARG;
        }
        const size_t added =
            cyber::retopo::applySymmetry(mesh->mesh, sym, snapperOf(snapper));
        if (out_added_faces != nullptr) {
            *out_added_faces = added;
        }
        return CYBER_OK;
    });
}

CyberStatus cyber_retopo_resymmetrize(CyberMesh* mesh, const CyberSymmetry* symmetry,
                                      float match_tolerance,
                                      CyberResymmetrizeReport* out_report) {
    return runMeshEdit(mesh, "cyber_retopo_resymmetrize", [&] {
        cyber::retopo::Symmetry sym;
        if (!toSymmetry(symmetry, sym)) {
            setError("cyber_retopo_resymmetrize: null or degenerate plane");
            return CYBER_ERR_INVALID_ARG;
        }
        const cyber::retopo::ResymmetrizeReport report =
            cyber::retopo::resymmetrize(mesh->mesh, sym, match_tolerance);
        if (out_report != nullptr) {
            out_report->snapped = static_cast<uint32_t>(report.snapped);
            out_report->matched = static_cast<uint32_t>(report.matched);
            out_report->unmatched = static_cast<uint32_t>(report.unmatched);
            out_report->max_correction = report.maxCorrection;
        }
        return CYBER_OK;
    });
}

// ---- EditMesh batch commands (retopology phase 4, task 4.5) ----------------
//
// See the ELEMENT-ID STABILITY block in cyber_capi.h: snap-all preserves
// every id, subdivide reassigns all of them, triangulate preserves vertex
// and edge ids while adding new face ids. The handle's overlay render state
// (hidden faces / tagged edges) is pruned accordingly, because it is keyed
// on exactly those ids.

CyberStatus cyber_retopo_snap_all(CyberMesh* mesh, const CyberSnapper* snapper,
                                  const uint32_t* pinned, size_t pinned_count,
                                  size_t* out_moved, float* out_max_distance) {
    return runMeshEdit(mesh, "cyber_retopo_snap_all", [&] {
        const cyber::retopo::SurfaceSnapper* snap = snapperOf(snapper);
        if (snap == nullptr || snap->empty()) {
            setError("cyber_retopo_snap_all: a non-empty Target snapper is required");
            return CYBER_ERR_INVALID_ARG;
        }
        const cyber::retopo::PinSet pins = makePinSet(pinned, pinned_count);
        size_t moved = 0;
        float maxDistance = 0.0f;
        for (std::uint32_t i = 0; i < mesh->mesh.vertexCapacity(); ++i) {
            const cyber::VertexId v{i};
            if (!mesh->mesh.isAlive(v) || pins.isPinned(v)) {
                continue;
            }
            const cyber::Vec3 before = mesh->mesh.position(v);
            const cyber::Vec3 after = snap->snapToSurface(before).point;
            const float distance = cyber::length(after - before);
            if (distance > 0.0f) {
                mesh->mesh.setPosition(v, after);
                ++moved;
                maxDistance = distance > maxDistance ? distance : maxDistance;
            }
        }
        if (out_moved != nullptr) {
            *out_moved = moved;
        }
        if (out_max_distance != nullptr) {
            *out_max_distance = maxDistance;
        }
        return CYBER_OK;
    });
}

CyberStatus cyber_retopo_subdivide(CyberMesh* mesh, const CyberSnapper* snapper,
                                   size_t* out_faces) {
    return runMeshEdit(mesh, "cyber_retopo_subdivide", [&] {
        if (mesh->mesh.faceCount() == 0) {
            setError("cyber_retopo_subdivide: mesh has no faces");
            return CYBER_ERR_EMPTY;
        }
        cyber::retopo::subdivideAll(mesh->mesh);
        // Reprojection: linear subdivision alone only inserts vertices on
        // the existing facets, so the Target projection is what recovers
        // curvature ("subdivide+reproject").
        const cyber::retopo::SurfaceSnapper* snap = snapperOf(snapper);
        if (snap != nullptr && !snap->empty()) {
            for (std::uint32_t i = 0; i < mesh->mesh.vertexCapacity(); ++i) {
                const cyber::VertexId v{i};
                if (mesh->mesh.isAlive(v)) {
                    mesh->mesh.setPosition(v, snap->snapToSurface(mesh->mesh.position(v)).point);
                }
            }
        }
        // Every id was reassigned: the handle's id-keyed overlay state is
        // meaningless now and would mis-hide/mis-tag unrelated elements.
        mesh->hiddenFaces.clear();
        mesh->taggedEdges.clear();
        if (out_faces != nullptr) {
            *out_faces = mesh->mesh.faceCount();
        }
        return CYBER_OK;
    });
}

CyberStatus cyber_retopo_triangulate(CyberMesh* mesh, size_t* out_faces) {
    return runMeshEdit(mesh, "cyber_retopo_triangulate", [&] {
        if (mesh->mesh.faceCount() == 0) {
            setError("cyber_retopo_triangulate: mesh has no faces");
            return CYBER_ERR_EMPTY;
        }
        mesh->mesh.triangulate();
        // Face ids survive only for the first ear of each n-gon, so a
        // hidden n-gon would come back partly visible: drop the hidden set.
        // Tagged EDGE ids are unaffected and are kept.
        mesh->hiddenFaces.clear();
        if (out_faces != nullptr) {
            *out_faces = mesh->mesh.faceCount();
        }
        return CYBER_OK;
    });
}

// ---- gesture stroke interpretation -----------------------------------------

// Opaque record: owns the C++ interpretation verbatim (ids in the record are
// engine element ids, valid as long as the elements stay alive).
struct CyberStrokeInterpretation {
    cyber::retopo::StrokeInterpretation record;
};

namespace {

CyberStrokeShape toCShape(cyber::retopo::StrokeShape shape) {
    using cyber::retopo::StrokeShape;
    switch (shape) {
        case StrokeShape::HoldPoint:
            return CYBER_SHAPE_HOLD_POINT;
        case StrokeShape::Line:
            return CYBER_SHAPE_LINE;
        case StrokeShape::ClosedLoop:
            return CYBER_SHAPE_CLOSED_LOOP;
        case StrokeShape::Circle:
            return CYBER_SHAPE_CIRCLE;
        case StrokeShape::Scribble:
            return CYBER_SHAPE_SCRIBBLE;
        case StrokeShape::Cross:
            return CYBER_SHAPE_CROSS;
        case StrokeShape::Lasso:
            return CYBER_SHAPE_LASSO;
        case StrokeShape::Grid:
            return CYBER_SHAPE_GRID;
        case StrokeShape::Unknown:
            break;
    }
    return CYBER_SHAPE_UNKNOWN;
}

CyberStrokeContext toCContext(cyber::retopo::UnderStroke context) {
    using cyber::retopo::UnderStroke;
    switch (context) {
        case UnderStroke::Face:
            return CYBER_CONTEXT_FACE;
        case UnderStroke::Edge:
            return CYBER_CONTEXT_EDGE;
        case UnderStroke::BoundaryEdge:
            return CYBER_CONTEXT_BOUNDARY_EDGE;
        case UnderStroke::Vertex:
            return CYBER_CONTEXT_VERTEX;
        case UnderStroke::EmptySurface:
            break;
    }
    return CYBER_CONTEXT_EMPTY_SURFACE;
}

CyberStrokeAction toCAction(cyber::retopo::InterpretedAction action) {
    using cyber::retopo::InterpretedAction;
    switch (action) {
        case InterpretedAction::CreateQuad:
            return CYBER_ACTION_CREATE_QUAD;
        case InterpretedAction::InsertLoop:
            return CYBER_ACTION_INSERT_LOOP;
        case InterpretedAction::TagLoop:
            return CYBER_ACTION_TAG_LOOP;
        case InterpretedAction::DissolveEdge:
            return CYBER_ACTION_DISSOLVE_EDGE;
        case InterpretedAction::DeleteFaces:
            return CYBER_ACTION_DELETE_FACES;
        case InterpretedAction::MergeVertices:
            return CYBER_ACTION_MERGE_VERTICES;
        case InterpretedAction::RotateEdge:
            return CYBER_ACTION_ROTATE_EDGE;
        case InterpretedAction::TweakVertex:
            return CYBER_ACTION_TWEAK_VERTEX;
        case InterpretedAction::HideRegion:
            return CYBER_ACTION_HIDE_REGION;
        case InterpretedAction::ToggleVisibility:
            return CYBER_ACTION_TOGGLE_VISIBILITY;
        case InterpretedAction::CreateGrid:
            return CYBER_ACTION_CREATE_GRID;
        case InterpretedAction::None:
            break;
    }
    return CYBER_ACTION_NONE;
}

CyberElementKind toCElementKind(cyber::retopo::ElementRef::Kind kind) {
    using Kind = cyber::retopo::ElementRef::Kind;
    switch (kind) {
        case Kind::Edge:
            return CYBER_ELEMENT_EDGE;
        case Kind::Face:
            return CYBER_ELEMENT_FACE;
        case Kind::Vertex:
            break;
    }
    return CYBER_ELEMENT_VERTEX;
}

const cyber::retopo::InterpretationCandidate* candidateAt(
    const CyberStrokeInterpretation* interpretation, size_t candidate) {
    if (interpretation == nullptr ||
        candidate >= interpretation->record.candidates.size()) {
        return nullptr;
    }
    return &interpretation->record.candidates[candidate];
}

}  // namespace

CyberStatus cyber_stroke_interpret(const CyberMesh* edit_mesh, const float* view_proj,
                                   const float* samples_xyt, size_t sample_count,
                                   float aspect, CyberStrokeInterpretation** out) {
    if (samples_xyt == nullptr || sample_count == 0 || out == nullptr) {
        setError("cyber_stroke_interpret: null/empty argument");
        return CYBER_ERR_INVALID_ARG;
    }
    if (edit_mesh != nullptr && view_proj == nullptr) {
        setError("cyber_stroke_interpret: view_proj required with edit_mesh");
        return CYBER_ERR_INVALID_ARG;
    }
    *out = nullptr;
    try {
        std::vector<cyber::retopo::ScreenSample> samples;
        samples.reserve(sample_count);
        for (size_t i = 0; i < sample_count; ++i) {
            samples.push_back({{samples_xyt[i * 3 + 0], samples_xyt[i * 3 + 1]},
                               samples_xyt[i * 3 + 2]});
        }
        cyber::retopo::ShapeParams params;
        if (aspect > 0.0f) {
            params.aspect = aspect;
        }
        auto handle = std::make_unique<CyberStrokeInterpretation>();
        handle->record = cyber::retopo::interpretStroke(
            samples, edit_mesh != nullptr ? &edit_mesh->mesh : nullptr, view_proj,
            params);
        clearError();
        *out = handle.release();
        return CYBER_OK;
    } catch (const std::exception& e) {
        setError(std::string("cyber_stroke_interpret: ") + e.what());
        return CYBER_ERR_RUNTIME;
    } catch (...) {
        setError("cyber_stroke_interpret: unknown error");
        return CYBER_ERR_RUNTIME;
    }
}

void cyber_stroke_interpretation_free(CyberStrokeInterpretation* interpretation) {
    delete interpretation;
}

CyberStrokeShape cyber_stroke_interpretation_shape(
    const CyberStrokeInterpretation* interpretation) {
    return interpretation == nullptr ? CYBER_SHAPE_UNKNOWN
                                     : toCShape(interpretation->record.shape.shape);
}

float cyber_stroke_interpretation_shape_confidence(
    const CyberStrokeInterpretation* interpretation) {
    return interpretation == nullptr ? 0.0f
                                     : interpretation->record.shape.confidence;
}

CyberStrokeContext cyber_stroke_interpretation_context(
    const CyberStrokeInterpretation* interpretation) {
    return interpretation == nullptr ? CYBER_CONTEXT_EMPTY_SURFACE
                                     : toCContext(interpretation->record.context);
}

size_t cyber_stroke_interpretation_candidate_count(
    const CyberStrokeInterpretation* interpretation) {
    return interpretation == nullptr ? 0 : interpretation->record.candidates.size();
}

CyberStrokeAction cyber_stroke_interpretation_action(
    const CyberStrokeInterpretation* interpretation, size_t candidate) {
    const auto* c = candidateAt(interpretation, candidate);
    return c == nullptr ? CYBER_ACTION_NONE : toCAction(c->action);
}

float cyber_stroke_interpretation_confidence(
    const CyberStrokeInterpretation* interpretation, size_t candidate) {
    const auto* c = candidateAt(interpretation, candidate);
    return c == nullptr ? 0.0f : c->confidence;
}

size_t cyber_stroke_interpretation_element_count(
    const CyberStrokeInterpretation* interpretation, size_t candidate) {
    const auto* c = candidateAt(interpretation, candidate);
    return c == nullptr ? 0 : c->elements.size();
}

int cyber_stroke_interpretation_element(const CyberStrokeInterpretation* interpretation,
                                        size_t candidate, size_t element,
                                        CyberElementKind* out_kind, uint32_t* out_id) {
    const auto* c = candidateAt(interpretation, candidate);
    if (c == nullptr || element >= c->elements.size()) {
        return 0;
    }
    const cyber::retopo::ElementRef& ref = c->elements[element];
    if (out_kind != nullptr) {
        *out_kind = toCElementKind(ref.kind);
    }
    if (out_id != nullptr) {
        *out_id = ref.id;
    }
    return 1;
}

size_t cyber_stroke_interpretation_corner_count(
    const CyberStrokeInterpretation* interpretation) {
    return interpretation == nullptr ? 0 : interpretation->record.shape.corners.size();
}

int cyber_stroke_interpretation_corner(const CyberStrokeInterpretation* interpretation,
                                       size_t corner, float out_xy[2]) {
    if (interpretation == nullptr || out_xy == nullptr ||
        corner >= interpretation->record.shape.corners.size()) {
        return 0;
    }
    const cyber::Vec2 c = interpretation->record.shape.corners[corner];
    out_xy[0] = c.x;
    out_xy[1] = c.y;
    return 1;
}

int cyber_stroke_interpretation_grid_size(const CyberStrokeInterpretation* interpretation,
                                          size_t* out_rows, size_t* out_cols) {
    if (interpretation == nullptr || interpretation->record.shape.gridRows <= 0 ||
        interpretation->record.shape.gridCols <= 0) {
        return 0;
    }
    if (out_rows != nullptr) {
        *out_rows = static_cast<size_t>(interpretation->record.shape.gridRows);
    }
    if (out_cols != nullptr) {
        *out_cols = static_cast<size_t>(interpretation->record.shape.gridCols);
    }
    return 1;
}

// ---- surface baking ------------------------------------------------------

struct CyberImage {
    cyber::bake::Image image;
};

void cyber_default_bake_params(CyberBakeParams* params) {
    if (params == nullptr) {
        return;
    }
    const cyber::bake::BakeParams d;
    params->width = d.width;
    params->height = d.height;
    params->cageDistance = d.cageDistance;
    params->aoSamples = d.aoSamples;
    params->aoRadius = d.aoRadius;
}

CyberStatus cyber_bake(const CyberMesh* low, const CyberMesh* high, CyberBakeMap map,
                       const CyberBakeParams* params, CyberImage** out) {
    if (low == nullptr || high == nullptr || out == nullptr) {
        setError("cyber_bake: null argument");
        return CYBER_ERR_INVALID_ARG;
    }
    try {
        cyber::bake::BakeParams p;
        if (params != nullptr) {
            p.width = params->width;
            p.height = params->height;
            p.cageDistance = params->cageDistance;
            p.aoSamples = params->aoSamples;
            p.aoRadius = params->aoRadius;
        }
        cyber::bake::BakeMap m{};
        switch (map) {
            case CYBER_BAKE_NORMAL:
                m = cyber::bake::BakeMap::Normal;
                break;
            case CYBER_BAKE_AO:
                m = cyber::bake::BakeMap::AmbientOcclusion;
                break;
            case CYBER_BAKE_DISPLACEMENT:
                m = cyber::bake::BakeMap::Displacement;
                break;
            case CYBER_BAKE_POSITION:
                m = cyber::bake::BakeMap::Position;
                break;
            case CYBER_BAKE_COLOR:
                m = cyber::bake::BakeMap::Color;
                break;
            default:
                setError("cyber_bake: unknown map type");
                return CYBER_ERR_INVALID_ARG;
        }
        cyber::bake::BakeResult result = cyber::bake::bake(low->mesh, high->mesh, m, p);
        if (result.image.pixels.empty()) {
            setError("cyber_bake: empty result (the low-poly needs UVs and the Target geometry)");
            return CYBER_ERR_EMPTY;
        }
        auto handle = std::make_unique<CyberImage>();
        handle->image = std::move(result.image);
        clearError();
        *out = handle.release();
        return CYBER_OK;
    } catch (const std::exception& e) {
        setError(std::string("cyber_bake: ") + e.what());
        return CYBER_ERR_RUNTIME;
    } catch (...) {
        setError("cyber_bake: unknown error");
        return CYBER_ERR_RUNTIME;
    }
}

void cyber_image_free(CyberImage* image) { delete image; }

int cyber_image_width(const CyberImage* image) { return image == nullptr ? 0 : image->image.width; }
int cyber_image_height(const CyberImage* image) {
    return image == nullptr ? 0 : image->image.height;
}
int cyber_image_channels(const CyberImage* image) {
    return image == nullptr ? 0 : image->image.channels;
}

size_t cyber_image_copy_pixels(const CyberImage* image, float* out, size_t max_floats) {
    if (image == nullptr) {
        return 0;
    }
    const std::vector<float>& px = image->image.pixels;
    if (out == nullptr) {
        return px.size();
    }
    const size_t n = std::min(px.size(), max_floats);
    std::copy(px.begin(), px.begin() + static_cast<std::ptrdiff_t>(n), out);
    return n;
}

CyberStatus cyber_image_save_png(const CyberImage* image, const char* path) {
    if (image == nullptr || path == nullptr) {
        setError("cyber_image_save_png: null argument");
        return CYBER_ERR_INVALID_ARG;
    }
    try {
        if (!cyber::imageio::saveImage(std::string(path), image->image,
                                       cyber::imageio::ImageFormat::Png)) {
            setError("cyber_image_save_png: write failed");
            return CYBER_ERR_IO;
        }
        clearError();
        return CYBER_OK;
    } catch (const std::exception& e) {
        setError(std::string("cyber_image_save_png: ") + e.what());
        return CYBER_ERR_IO;
    } catch (...) {
        setError("cyber_image_save_png: unknown error");
        return CYBER_ERR_IO;
    }
}
