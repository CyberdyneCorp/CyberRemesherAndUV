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
#ifdef CYBER_CAPI_WITH_UV
#include "cyber/uv/atlas.hpp"
#endif

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
    // quad-cover is the default: it reaches QuadriFlow-class irregular/CV where a solver
    // is available, and degrades to field-aligned (per-island) when it is not.
    params->quadMethod = CYBER_QUAD_QUADCOVER;
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
        // Poll the C cancel callback directly from isCancelled(), so a long report-less
        // stage (e.g. the native seamless-UV solve) is cancellable mid-flight, not only at
        // the progress-report boundaries makeSink flips the flag on.
        token.setPoll([cancel, user]() { return cancel != nullptr && cancel(user) != 0; });
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
        // quad-cover is the default, but it needs a seamless-UV solver (in-process build
        // or the CYBER_QUADCOVER_CLI harness). When neither is present, fall back to the
        // field-aligned quadrangulator so a default build still produces output; when it
        // IS present, pass field-aligned as the per-island fallback the pipeline uses if
        // quad-cover declines an island.
        int quadMethod = params->quadMethod;
        if (quadMethod == CYBER_QUAD_QUADCOVER && !cyber::remesh::quadCoverAvailable()) {
            quadMethod = CYBER_QUAD_FIELD_ALIGNED;
        }
        const auto makeQuad = [](int method) -> std::unique_ptr<cyber::remesh::IQuadrangulator> {
            if (method == CYBER_QUAD_INSTANT_MESHES) {
                return cyber::remesh::makeInstantMeshesQuadrangulator();
            }
            if (method == CYBER_QUAD_INTEGER) {
                return cyber::remesh::makeIntegerQuadrangulator();
            }
            if (method == CYBER_QUAD_QUADCOVER) {
                // Uniform (adaptivity 0): quad-cover is a seamless global-grid method whose
                // strength is uniform clean topology. Measured, curvature-adaptive sizing on
                // it gives no surface-fidelity gain but injects singularities and edge-length
                // variance (spot irr 2->6%, fandisk 3->15%), so — unlike the field/integer
                // paths — it stays uniform-only here. The adaptivity knob remains available
                // for experiments via makeQuadCoverQuadrangulator(iters, a) / CYBER_QC_ADAPT.
                return cyber::remesh::makeQuadCoverQuadrangulator(40, 0.0f);
            }
            return cyber::remesh::makeFieldAlignedQuadrangulator();
        };
        cyber::remesh::QuadrangulatorFactory fallback;
        if (quadMethod == CYBER_QUAD_QUADCOVER) {
            fallback = []() { return cyber::remesh::makeFieldAlignedQuadrangulator(); };
        }
        cyber::remesh::PipelineResult result = cyber::remesh::remesh(
            in->mesh, cppParams, &sink, &token,
            [quadMethod, makeQuad]() { return makeQuad(quadMethod); }, fallback);

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

void cyber_default_atlas_params(CyberAtlasParams* params) {
    if (params == nullptr) {
        return;
    }
#ifdef CYBER_CAPI_WITH_UV
    const cyber::uv::AtlasOptions defaults;
    params->maxChartAngleDegrees = defaults.maxChartAngleDeg;
    params->packMargin = defaults.pack.margin;
    params->textureSize = defaults.pack.textureSize;
    params->reorientCharts = defaults.reorientCharts ? 1 : 0;
    params->mergeCharts = defaults.mergeCharts ? 1 : 0;
#else
    params->maxChartAngleDegrees = 40.0f;
    params->packMargin = 0.0f;
    params->textureSize = 1024;
    params->reorientCharts = 1;
    params->mergeCharts = 1;
#endif
}

CyberStatus cyber_uv_atlas([[maybe_unused]] CyberMesh* mesh,
                           [[maybe_unused]] const CyberAtlasParams* params,
                           [[maybe_unused]] CyberAtlasResult* out) {
#ifdef CYBER_CAPI_WITH_UV
    if (mesh == nullptr) {
        setError("cyber_uv_atlas: null mesh");
        return CYBER_ERR_INVALID_ARG;
    }
    try {
        cyber::uv::AtlasOptions opts;
        if (params != nullptr) {
            opts.maxChartAngleDeg = params->maxChartAngleDegrees;
            opts.pack.margin = params->packMargin;
            opts.pack.textureSize = params->textureSize;
            opts.reorientCharts = params->reorientCharts != 0;
            opts.mergeCharts = params->mergeCharts != 0;
        }
        const cyber::uv::AtlasResult r = cyber::uv::unwrapAtlas(mesh->mesh, opts);
        if (out != nullptr) {
            out->chartCount = r.chartCount;
            out->seamEdges = r.seamEdges;
            out->maxAngleDistortion = r.maxAngleDistortion;
            out->rmsAngleDistortion = r.rmsAngleDistortion;
            out->flippedCharts = r.flippedCharts;
            out->fallbackCharts = r.fallbackCharts;
            out->packedArea = r.packedArea;
            out->texelDensity = r.texelDensity;
        }
        clearError();
        return r.ok ? CYBER_OK : CYBER_ERR_RUNTIME;
    } catch (const std::exception& e) {
        setError(std::string("cyber_uv_atlas: ") + e.what());
        return CYBER_ERR_RUNTIME;
    } catch (...) {
        setError("cyber_uv_atlas: unknown error");
        return CYBER_ERR_RUNTIME;
    }
#else
    setError("cyber_uv_atlas: engine built without the UV module (CYBER_BUILD_UV=OFF)");
    return CYBER_ERR_RUNTIME;
#endif
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
