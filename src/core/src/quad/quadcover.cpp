#include "cyber/core/quad/quadcover.hpp"

#include <exploragram/hexdom/quad_cover.h>
#include <geogram/basic/attributes.h>
#include <geogram/basic/common.h>
#include <geogram/basic/logger.h>
#include <geogram/mesh/mesh.h>
#include <geogram/mesh/mesh_frame_field.h>
#include <geogram_report_progress.h>

#include <cmath>
#include <exception>
#include <mutex>
#include <vector>

namespace cyber::remesh::quad {

namespace {

void ensureGeogramInitialized() {
    static std::once_flag once;
    std::call_once(once, [] {
        GEO::initialize();  // internally idempotent
        // The solvers log matrix sizes etc. to stdout; keep engine output
        // clean (diagnostics spec: quiet by default).
        GEO::Logger::instance()->set_quiet(true);
    });
}

// Index mappings between the cyber mesh (sparse id spaces) and the dense
// GEO::Mesh built from it.
struct GeoMaps {
    std::vector<VertexId> vertexOfGeo;  // geo vertex -> cyber vertex
    std::vector<FaceId> faceOfGeo;      // geo facet  -> cyber face
};

// Builds a GEO::Mesh from a triangulated island. Ported from AutoRemesher's
// Parameterizer (MIT, Jeremy HU 2020), including the both-sides adjacency
// clear for inconsistently-oriented/non-manifold edges — without it,
// quad_cover's adjacency assertion fires on such input.
GeoMaps buildGeoMesh(const Mesh& mesh, GEO::Mesh& out) {
    GeoMaps maps;
    out.vertices.set_dimension(3);

    std::vector<GEO::index_t> geoOfVertex(mesh.vertexCapacity(), GEO::NO_VERTEX);
    for (Index i = 0; i < mesh.vertexCapacity(); ++i) {
        const VertexId v{i};
        if (!mesh.isAlive(v)) {
            continue;
        }
        const GEO::index_t gv = out.vertices.create_vertex();
        geoOfVertex[i] = gv;
        maps.vertexOfGeo.push_back(v);
        const Vec3 p = mesh.position(v);
        double* dst = out.vertices.point_ptr(gv);
        dst[0] = static_cast<double>(p.x);
        dst[1] = static_cast<double>(p.y);
        dst[2] = static_cast<double>(p.z);
    }

    for (Index i = 0; i < mesh.faceCapacity(); ++i) {
        const FaceId f{i};
        if (!mesh.isAlive(f)) {
            continue;
        }
        const auto verts = mesh.faceVertices(f);  // triangulated input
        const GEO::index_t gf = out.facets.create_polygon(3);
        for (GEO::index_t lv = 0; lv < 3; ++lv) {
            out.facets.set_vertex(gf, lv, geoOfVertex[verts[lv].value]);
        }
        maps.faceOfGeo.push_back(f);
    }
    out.facets.connect();

    for (GEO::index_t c = 0; c < out.facet_corners.nb(); ++c) {
        const GEO::index_t f2 = out.facet_corners.adjacent_facet(c);
        if (f2 == GEO::NO_FACET) {
            continue;
        }
        const GEO::index_t f1 = c / 3;
        const GEO::index_t e2 = out.facets.find_adjacent(f2, f1);
        if (e2 == GEO::NO_FACET) {
            out.facet_corners.set_adjacent_facet(c, GEO::NO_FACET);
            continue;
        }
        const GEO::index_t c2 = out.facets.corners_begin(f2) + e2;
        const GEO::index_t c3 = out.facets.next_corner_around_facet(f2, c2);
        if (out.facet_corners.vertex(c) != out.facet_corners.vertex(c3)) {
            out.facet_corners.set_adjacent_facet(c, GEO::NO_FACET);
            out.facet_corners.set_adjacent_facet(c2, GEO::NO_FACET);
        }
    }
    return maps;
}

float averageEdgeLength(const Mesh& mesh) {
    double sum = 0.0;
    std::size_t count = 0;
    for (Index i = 0; i < mesh.edgeCapacity(); ++i) {
        const EdgeId e{i};
        if (!mesh.isAlive(e)) {
            continue;
        }
        const auto [a, b] = mesh.edgeVertices(e);
        sum += static_cast<double>(length(mesh.position(a) - mesh.position(b)));
        ++count;
    }
    return count > 0 ? static_cast<float>(sum / static_cast<double>(count)) : 0.0f;
}

// RAII binding of the slice's thread_local progress hooks to a ProgressSink.
// quad_cover reports (round 0..7, progress 0..1) through these; they are
// thread_local, so concurrent island solves stay instance-local (the fix for
// AutoRemesher's global-progress spinlock).
class ProgressHookGuard {
public:
    explicit ProgressHookGuard(ProgressSink* sink) : m_sink(sink) {
        geogram_report_progress_tag = this;
        geogram_report_progress_callback = &ProgressHookGuard::onProgress;
    }
    ~ProgressHookGuard() {
        geogram_report_progress_tag = nullptr;
        geogram_report_progress_callback = nullptr;
    }
    ProgressHookGuard(const ProgressHookGuard&) = delete;
    ProgressHookGuard& operator=(const ProgressHookGuard&) = delete;

private:
    static void onProgress(void* tag, float progress) {
        auto* self = static_cast<ProgressHookGuard*>(tag);
        if (self == nullptr || self->m_sink == nullptr) {
            return;
        }
        constexpr float kRounds = 8.0f;
        const float round = static_cast<float>(geogram_report_progress_round);
        self->m_sink->report((round + progress) / kRounds, "parameterize");
    }

    ProgressSink* m_sink;
};

class QuadCoverFrameFieldSolver final : public IFrameFieldSolver {
public:
    explicit QuadCoverFrameFieldSolver(QuadCoverOptions options) : m_options(options) {}

    // `features` is unused: angle re-detection is deferred until 5.10. `vertexScales`
    // is unused too — density enters the parameterization, not the field.
    Outcome solve(const Mesh& mesh, const FeatureGraph& /*features*/,
                  const std::vector<float>& /*vertexScales*/, ProgressSink* progress,
                  const CancelToken* cancel) override {
        ensureGeogramInitialized();
        Outcome outcome;
        if (cancel && cancel->isCancelled()) {
            outcome.failureReason = "cancelled";
            return outcome;
        }
        try {
            GEO::Mesh geo;
            const GeoMaps maps = buildGeoMesh(mesh, geo);

            // geogram's FrameField uses the process-global progress task;
            // serialize this (short) section across threads.
            static std::mutex frameFieldMutex;
            GEO::FrameField field;
            {
                const std::lock_guard<std::mutex> lock(frameFieldMutex);
                field.set_use_spatial_search(false);
                field.create_from_surface_mesh(geo, false,
                                               static_cast<double>(m_options.sharpEdgeDegrees));
            }

            // One representative cross direction per face: the first row of
            // geogram's 3x3 frame (the only row quad_cover consumes).
            outcome.field.faceDirections.assign(mesh.faceCapacity(), Vec3{});
            const auto& frames = field.frames();
            for (std::size_t gf = 0; gf < maps.faceOfGeo.size(); ++gf) {
                outcome.field.faceDirections[maps.faceOfGeo[gf].value] = Vec3{
                    static_cast<float>(frames[9 * gf + 0]), static_cast<float>(frames[9 * gf + 1]),
                    static_cast<float>(frames[9 * gf + 2])};
            }
            outcome.success = true;
        } catch (const std::exception& e) {
            outcome.failureReason = e.what();
        } catch (...) {
            outcome.failureReason = "frame field solve failed";
        }
        if (progress && outcome.success) {
            progress->report(1.0f, "frame field");
        }
        return outcome;
    }

    [[nodiscard]] std::string name() const override { return "quadcover-framefield"; }

private:
    QuadCoverOptions m_options;
};

class QuadCoverParameterizer final : public IParameterizer {
public:
    explicit QuadCoverParameterizer(QuadCoverOptions options) : m_options(options) {}

    Outcome parameterize(const Mesh& mesh, const FeatureGraph& /*features — see 5.10*/,
                         const FrameField& field, float targetEdgeLength, ProgressSink* progress,
                         const CancelToken* cancel) override {
        ensureGeogramInitialized();
        Outcome outcome;
        if (cancel && cancel->isCancelled()) {
            outcome.cancelled = true;
            return outcome;
        }
        const float avgEdge = averageEdgeLength(mesh);
        if (avgEdge <= 0.0f || targetEdgeLength <= 0.0f) {
            outcome.failureReason = "degenerate island (no edges or non-positive target)";
            return outcome;
        }
        try {
            GEO::Mesh geo;
            const GeoMaps maps = buildGeoMesh(mesh, geo);

            GEO::Attribute<GEO::vec3> fieldAttr(geo.facets.attributes(), "B");
            for (std::size_t gf = 0; gf < maps.faceOfGeo.size(); ++gf) {
                const Vec3 d = field.faceDirections[maps.faceOfGeo[gf].value];
                fieldAttr[static_cast<GEO::index_t>(gf)] = GEO::vec3(
                    static_cast<double>(d.x), static_cast<double>(d.y), static_cast<double>(d.z));
            }

            GEO::Attribute<GEO::vec2> uv(geo.facet_corners.attributes(), "U");

            // quad_cover's scaling is expressed as a multiple of the average
            // edge length; requesting target/avg makes the integer isoline
            // spacing land at targetEdgeLength in world units.
            const double scaling =
                static_cast<double>(targetEdgeLength) / static_cast<double>(avgEdge);

            {
                ProgressHookGuard hooks(progress);
                GEO::GlobalParam2d::quad_cover(&geo, fieldAttr, uv, scaling,
                                               /*constrain_hard_edges=*/true,
                                               /*do_brush=*/true, /*integer_constraints=*/true,
                                               static_cast<double>(m_options.sharpEdgeDegrees));
            }
            if (cancel && cancel->isCancelled()) {
                outcome.cancelled = true;
                return outcome;
            }

            outcome.param.cornerUVs.assign(mesh.faceCapacity(), {});
            for (std::size_t gf = 0; gf < maps.faceOfGeo.size(); ++gf) {
                auto& corners = outcome.param.cornerUVs[maps.faceOfGeo[gf].value];
                const GEO::index_t base = geo.facets.corners_begin(static_cast<GEO::index_t>(gf));
                for (GEO::index_t k = 0; k < 3; ++k) {
                    const GEO::vec2 value = uv[base + k];
                    corners[k] = Vec2{static_cast<float>(value.x), static_cast<float>(value.y)};
                    if (!std::isfinite(value.x) || !std::isfinite(value.y)) {
                        outcome.failureReason = "non-finite UV in quad_cover solution";
                        return outcome;
                    }
                }
            }

            GEO::Attribute<bool> singular;
            singular.bind_if_is_defined(geo.vertices.attributes(), "is_singular");
            if (singular.is_bound()) {
                for (std::size_t gv = 0; gv < maps.vertexOfGeo.size(); ++gv) {
                    if (singular[static_cast<GEO::index_t>(gv)]) {
                        outcome.param.singularVertices.push_back(maps.vertexOfGeo[gv]);
                    }
                }
            }

            // integer_constraints=true: the solve already snapped isolines
            // (MIQ-style rounding) — this is the baseline quantizer of D2;
            // the Bi-MDF quantizer (5.11) will consume integer=false output.
            outcome.param.integer = true;
            outcome.success = true;
        } catch (const std::exception& e) {
            outcome.failureReason = e.what();
        } catch (...) {
            outcome.failureReason = "quad_cover parameterization failed";
        }
        return outcome;
    }

    [[nodiscard]] std::string name() const override { return "quadcover-miq"; }

private:
    QuadCoverOptions m_options;
};

}  // namespace

std::unique_ptr<IFrameFieldSolver> makeQuadCoverFrameFieldSolver(QuadCoverOptions options) {
    return std::make_unique<QuadCoverFrameFieldSolver>(options);
}

std::unique_ptr<IParameterizer> makeQuadCoverParameterizer(QuadCoverOptions options) {
    return std::make_unique<QuadCoverParameterizer>(options);
}

}  // namespace cyber::remesh::quad
