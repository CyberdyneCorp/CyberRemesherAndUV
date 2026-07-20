#include "cyber/quadrangulate/quadcover_extractor.hpp"

#include <atomic>
#include <cmath>
#include <cstdio>
#include <cstdlib>
#include <filesystem>
#include <map>
#include <memory>
#include <string>
#include <utility>
#include <vector>

// TASK F. The isoline extractor and the IQuadrangulator seam are still stubs
// (Milestones 2-3); Milestone 1 fills computeSeamlessUv by obtaining a seamless
// integer-grid UV out-of-process from AutoRemesher's Geogram quad_cover harness.
namespace cyber::remesh {

namespace {

// Parse the harness UV dump (autoremesher_harness.cpp writeUvDump): the isotropic
// mesh (V) plus per-triangle vertex indices and 3 corner UVs (T).
bool parseUvDump(const std::string& path, SeamlessUv& uv) {
    std::FILE* f = std::fopen(path.c_str(), "rb");
    if (f == nullptr) {
        return false;
    }
    char line[1024];
    while (std::fgets(line, sizeof(line), f) != nullptr) {
        if (line[0] == 'V') {
            long n = 0;
            std::sscanf(line + 1, "%ld", &n);
            for (long i = 0; i < n; ++i) {
                if (std::fgets(line, sizeof(line), f) == nullptr) {
                    std::fclose(f);
                    return false;
                }
                double x = 0, y = 0, z = 0;
                std::sscanf(line, "%lf %lf %lf", &x, &y, &z);
                uv.vertices.push_back(Vec3{static_cast<float>(x), static_cast<float>(y),
                                           static_cast<float>(z)});
            }
        } else if (line[0] == 'T') {
            long n = 0;
            std::sscanf(line + 1, "%ld", &n);
            for (long i = 0; i < n; ++i) {
                if (std::fgets(line, sizeof(line), f) == nullptr) {
                    std::fclose(f);
                    return false;
                }
                long a = 0, b = 0, c = 0;
                double u0 = 0, v0 = 0, u1 = 0, v1 = 0, u2 = 0, v2 = 0;
                std::sscanf(line, "%ld %ld %ld %lf %lf %lf %lf %lf %lf", &a, &b, &c, &u0, &v0, &u1,
                            &v1, &u2, &v2);
                uv.triangles.push_back({static_cast<Index>(a), static_cast<Index>(b),
                                        static_cast<Index>(c)});
                uv.triangleUv.push_back({Vec2{static_cast<float>(u0), static_cast<float>(v0)},
                                         Vec2{static_cast<float>(u1), static_cast<float>(v1)},
                                         Vec2{static_cast<float>(u2), static_cast<float>(v2)}});
            }
        }
    }
    std::fclose(f);
    return !uv.triangles.empty();
}

// Write a minimal Wavefront OBJ (triangles) for the harness to consume.
bool writeObjFor(const std::string& path, const Mesh& mesh, double& areaOut) {
    std::vector<Vec3> pos;
    std::vector<std::vector<Index>> faces;
    mesh.toIndexed(pos, faces);
    std::FILE* f = std::fopen(path.c_str(), "wb");
    if (f == nullptr) {
        return false;
    }
    for (const Vec3& p : pos) {
        std::fprintf(f, "v %.9g %.9g %.9g\n", static_cast<double>(p.x), static_cast<double>(p.y),
                     static_cast<double>(p.z));
    }
    double area = 0.0;
    for (const auto& fc : faces) {
        std::fprintf(f, "f");
        for (const Index v : fc) {
            std::fprintf(f, " %u", static_cast<unsigned>(v) + 1U);
        }
        std::fprintf(f, "\n");
        for (std::size_t k = 1; k + 1 < fc.size(); ++k) {
            area += 0.5 * static_cast<double>(length(cross(pos[fc[k]] - pos[fc[0]],
                                                           pos[fc[k + 1]] - pos[fc[0]])));
        }
    }
    std::fclose(f);
    areaOut = area;
    return true;
}

}  // namespace

SeamlessUv computeSeamlessUv(const Mesh& mesh, float targetEdgeLength) {
    SeamlessUv uv;
    const char* cli = std::getenv("CYBER_QUADCOVER_CLI");
    if (cli == nullptr || targetEdgeLength <= 0.0f) {
        return uv;  // no solver available -> caller degrades cleanly
    }
    static std::atomic<unsigned> counter{0};
    const std::filesystem::path dir = std::filesystem::temp_directory_path();
    const std::string tag = "cyber_qc_" + std::to_string(counter.fetch_add(1));
    const std::string objPath = (dir / (tag + ".obj")).string();
    const std::string uvPath = (dir / (tag + ".uv")).string();

    double area = 0.0;
    if (!writeObjFor(objPath, mesh, area)) {
        return uv;
    }
    const double cell = static_cast<double>(targetEdgeLength) * static_cast<double>(targetEdgeLength);
    const long quads = cell > 0.0 ? std::max(64L, std::lround(area / cell)) : 3000L;
    const std::string cmd = std::string(cli) + " -i " + objPath + " -u " + uvPath + " -f " +
                            std::to_string(quads) + " -s 0.5 >/dev/null 2>&1";
    const int rc = std::system(cmd.c_str());
    if (rc == 0) {
        uv.valid = parseUvDump(uvPath, uv);
    }
    std::error_code ec;
    std::filesystem::remove(objPath, ec);
    std::filesystem::remove(uvPath, ec);
    return uv;
}

// Max integer-jump residual across interior edges: for each shared edge the grid
// symmetry mapping one triangle's shared-vertex UVs to the other's must have an
// integer translation. 0 == seamless. (Mirrors the M1 python validator.)
double seamlessUvResidual(const SeamlessUv& uv) {
    if (!uv.valid || uv.triangles.empty()) {
        return 0.0;
    }
    std::map<std::pair<Index, Index>, std::vector<int>> edges;
    for (int t = 0; t < static_cast<int>(uv.triangles.size()); ++t) {
        const auto& tri = uv.triangles[static_cast<std::size_t>(t)];
        for (int i = 0; i < 3; ++i) {
            const Index a = tri[static_cast<std::size_t>(i)];
            const Index b = tri[static_cast<std::size_t>((i + 1) % 3)];
            edges[{std::min(a, b), std::max(a, b)}].push_back(t);
        }
    }
    const auto uvOf = [&](int t, Index vid) {
        const auto& tri = uv.triangles[static_cast<std::size_t>(t)];
        for (int c = 0; c < 3; ++c) {
            if (tri[static_cast<std::size_t>(c)] == vid) {
                return uv.triangleUv[static_cast<std::size_t>(t)][static_cast<std::size_t>(c)];
            }
        }
        return Vec2{0.0f, 0.0f};
    };
    const auto rot90 = [](Vec2 v, int k) {
        for (int i = 0; i < ((k % 4) + 4) % 4; ++i) {
            v = Vec2{-v.y, v.x};
        }
        return v;
    };
    double maxRes = 0.0;
    for (const auto& [e, inc] : edges) {
        if (inc.size() != 2) {
            continue;
        }
        const Vec2 a1 = uvOf(inc[0], e.first), a2 = uvOf(inc[0], e.second);
        const Vec2 b1 = uvOf(inc[1], e.first), b2 = uvOf(inc[1], e.second);
        const Vec2 da{a2.x - a1.x, a2.y - a1.y}, db{b2.x - b1.x, b2.y - b1.y};
        if (da.x == 0.0f && da.y == 0.0f) {
            continue;
        }
        int bestK = 0;
        float bestD = 1e18f;
        for (int k = 0; k < 4; ++k) {
            const Vec2 r = rot90(da, k);
            const float d = (r.x - db.x) * (r.x - db.x) + (r.y - db.y) * (r.y - db.y);
            if (d < bestD) {
                bestD = d;
                bestK = k;
            }
        }
        const Vec2 ra1 = rot90(a1, bestK);
        const float tx = b1.x - ra1.x, ty = b1.y - ra1.y;
        maxRes = std::max(maxRes, static_cast<double>(std::max(std::abs(tx - std::round(tx)),
                                                               std::abs(ty - std::round(ty)))));
    }
    return maxRes;
}

// Collaborator 2 stub: with no seamless UV there are no isolines to trace, so
// return an empty mesh. Milestone 2 ports quadextractor.cpp here.
IsolineQuadMesh extractIsolineQuads(const Mesh& /*mesh*/, const SeamlessUv& /*uv*/) {
    return IsolineQuadMesh{};
}

namespace {

// IQuadrangulator implementation. The stub does not touch `mesh`: on the failure
// path the caller keeps its input triangle island unchanged, which is exactly the
// safe degrade behaviour the pipeline expects from a not-yet-implemented seam.
class QuadCoverQuadrangulator final : public IQuadrangulator {
public:
    explicit QuadCoverQuadrangulator(int fieldIterations) : m_fieldIterations(fieldIterations) {}

    Outcome quadrangulate(Mesh& mesh, float targetEdgeLength, ProgressSink* progress,
                          const CancelToken* cancel) override {
        if (cancel != nullptr && cancel->isCancelled()) {
            return {.success = false, .cancelled = true, .failureReason = {}};
        }
        if (progress != nullptr) {
            progress->report(0.0f, "quadrangulate (quad-cover: not implemented)");
        }

        // --- Real pipeline goes here (Milestones 1-4) ---
        //   SeamlessUv uv = computeSeamlessUv(mesh, targetEdgeLength);
        //   if (!uv.valid) return { .success = false, ... };
        //   IsolineQuadMesh out = extractIsolineQuads(mesh, uv);
        //   rewriteMeshInPlace(mesh, out);   // replace tris with quads
        // Until then, leave `mesh` untouched and report not-implemented.
        (void)mesh;
        (void)targetEdgeLength;
        (void)m_fieldIterations;

        return {.success = false,
                .cancelled = false,
                .failureReason = "quad-cover isoline extractor not implemented (Task F scaffold)"};
    }

    [[nodiscard]] std::string name() const override { return "quad-cover"; }

private:
    int m_fieldIterations;
};

}  // namespace

std::unique_ptr<IQuadrangulator> makeQuadCoverQuadrangulator(int fieldIterations) {
    return std::make_unique<QuadCoverQuadrangulator>(fieldIterations);
}

}  // namespace cyber::remesh
