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
#include <exception>
#include <filesystem>
#include <memory>
#include <vector>
#include <optional>
#include <string>
#include <string_view>

#include "cyber/core/io.hpp"
#include "cyber/core/mesh.hpp"
#include "cyber/core/pipeline.hpp"
#include "cyber/core/progress.hpp"
#include "cyber/core/remesh_params.hpp"

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

// Opaque handle definition: the mesh plus any statistics captured when the
// handle was produced by cyber_remesh (islandsFailed cannot be recovered
// from geometry alone).
struct CyberMesh {
    cyber::Mesh mesh;
    std::optional<cyber::remesh::Statistics> stats;
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

        cyber::remesh::PipelineResult result =
            cyber::remesh::remesh(in->mesh, cppParams, &sink, &token);

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
                return CYBER_ERR_RUNTIME;
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
    mesh->mesh.toIndexed(positions, faces);
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
