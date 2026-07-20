#include "cyber/quadrangulate/quadcover_extractor.hpp"

#include <algorithm>
#include <array>
#include <atomic>
#include <cmath>
#include <cstddef>
#include <cstdint>
#include <cstdio>
#include <cstdlib>
#include <filesystem>
#include <limits>
#include <map>
#include <memory>
#include <queue>
#include <set>
#include <string>
#include <tuple>
#include <unordered_map>
#include <unordered_set>
#include <utility>
#include <vector>

#ifdef CYBER_HAVE_QUADCOVER
#include "autoremesher_solve.hpp"
#endif

// TASK F. The isoline extractor and the IQuadrangulator seam are still stubs
// (Milestones 2-3); Milestone 1 fills computeSeamlessUv by obtaining a seamless
// integer-grid UV out-of-process from AutoRemesher's Geogram quad_cover harness.
namespace cyber::remesh {

namespace {

#ifndef CYBER_HAVE_QUADCOVER
// The OBJ writer and UV-dump parser serve only the out-of-process subprocess path.
// With the in-process solver linked in they are unused, so compile them out to keep
// -Werror=unused-function happy.

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
#endif  // CYBER_HAVE_QUADCOVER

}  // namespace

SeamlessUv computeSeamlessUv(const Mesh& mesh, float targetEdgeLength, float harnessScaling) {
    SeamlessUv uv;
    if (targetEdgeLength <= 0.0f) {
        return uv;  // no target density -> caller degrades cleanly
    }

#ifdef CYBER_HAVE_QUADCOVER
    // In-process solve (M4c): no subprocess, no temp files, no CYBER_QUADCOVER_CLI.
    // Convert the mesh to indexed verts/tris, derive the target quad count from the
    // requested edge length exactly as the subprocess path did, and run AutoRemesher's
    // Geogram quad_cover directly via the isolated cyber_quadcover_solver library.
    std::vector<Vec3> pos;
    std::vector<std::vector<Index>> faces;
    mesh.toIndexed(pos, faces);
    if (pos.empty() || faces.empty()) {
        return uv;
    }
    std::vector<std::array<double, 3>> verts;
    verts.reserve(pos.size());
    for (const Vec3& p : pos) {
        verts.push_back({static_cast<double>(p.x), static_cast<double>(p.y),
                         static_cast<double>(p.z)});
    }
    std::vector<std::array<std::size_t, 3>> tris;
    double area = 0.0;
    for (const auto& fc : faces) {
        for (std::size_t k = 1; k + 1 < fc.size(); ++k) {  // fan-triangulate
            tris.push_back({static_cast<std::size_t>(fc[0]), static_cast<std::size_t>(fc[k]),
                            static_cast<std::size_t>(fc[k + 1])});
            area += 0.5 * static_cast<double>(length(cross(pos[fc[k]] - pos[fc[0]],
                                                           pos[fc[k + 1]] - pos[fc[0]])));
        }
    }
    if (tris.empty()) {
        return uv;
    }
    const double cell =
        static_cast<double>(targetEdgeLength) * static_cast<double>(targetEdgeLength);
    const long quads = cell > 0.0 ? std::max(64L, std::lround(area / cell)) : 3000L;
    // Scaling defaults to the passed harnessScaling (the QuadCoverQuadrangulator
    // calibration loop sweeps it); CYBER_QC_SCALING overrides for calibration sweeps.
    // Adaptivity defaults to a uniform field (0.0) matching the subprocess `-a 0.0`;
    // CYBER_QC_ADAPT overrides. Both env overrides honored in BOTH paths.
    const char* scalingEnv = std::getenv("CYBER_QC_SCALING");
    const double scaling =
        scalingEnv != nullptr ? std::atof(scalingEnv) : static_cast<double>(harnessScaling);
    const char* adaptEnv = std::getenv("CYBER_QC_ADAPT");
    const double adapt = adaptEnv != nullptr ? std::atof(adaptEnv) : 0.0;
    const bool dbg = std::getenv("CYBER_QC_DEBUG") != nullptr;
    if (dbg) {
        std::fprintf(stderr, "[qc] in-process edgeLen=%g area=%g quads=%ld s=%g a=%g\n",
                     static_cast<double>(targetEdgeLength), area, quads, scaling, adapt);
    }
    const qcsolver::SeamlessSolveResult res =
        qcsolver::solveSeamlessUv(verts, tris, quads, scaling, adapt);
    if (dbg) {
        std::fprintf(stderr, "[qc] in-process ok=%d isoTris=%zu\n", static_cast<int>(res.ok),
                     res.triangles.size());
    }
    if (!res.ok) {
        return uv;
    }
    uv.vertices.reserve(res.vertices.size());
    for (const auto& v : res.vertices) {
        uv.vertices.push_back(Vec3{static_cast<float>(v[0]), static_cast<float>(v[1]),
                                   static_cast<float>(v[2])});
    }
    uv.triangles.reserve(res.triangles.size());
    uv.triangleUv.reserve(res.triangleUvs.size());
    for (std::size_t t = 0; t < res.triangles.size(); ++t) {
        const auto& tri = res.triangles[t];
        const auto& q = res.triangleUvs[t];
        uv.triangles.push_back({static_cast<Index>(tri[0]), static_cast<Index>(tri[1]),
                                static_cast<Index>(tri[2])});
        uv.triangleUv.push_back({Vec2{static_cast<float>(q[0]), static_cast<float>(q[1])},
                                 Vec2{static_cast<float>(q[2]), static_cast<float>(q[3])},
                                 Vec2{static_cast<float>(q[4]), static_cast<float>(q[5])}});
    }
    uv.valid = !uv.triangles.empty();
    return uv;
#else
    const char* cli = std::getenv("CYBER_QUADCOVER_CLI");
    if (cli == nullptr) {
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
    // The harness scaling controls the isotropic-remesh density that in turn sets the
    // seamless-UV grid resolution (and thus our extracted quad count). Overridable via
    // CYBER_QC_SCALING for calibration sweeps; the default is tuned so the extracted quad
    // count tracks the requested target (see M4 calibration in docs/quadcover-plan.md).
    const char* scalingEnv = std::getenv("CYBER_QC_SCALING");
    const std::string scaling =
        scalingEnv != nullptr ? std::string(scalingEnv) : std::to_string(harnessScaling);
    // Frame-field gradient adaptivity (harness -a). AutoRemesher defaults to 1.0, whose
    // curvature-driven scaling gradients inject singularities; a UNIFORM field (0.0)
    // minimizes them and is the right choice here since quad-cover is a uniform-density
    // method (it bypasses the pipeline's adaptive isotropic stage). Measured at matched
    // density this drops irregular fraction — spot 3.6->2.6%, fandisk 5.2->1.6% — and
    // lifts quad-dominance to ~99.6% (M4b). CYBER_QC_ADAPT overrides for sweeps.
    const char* adaptEnv = std::getenv("CYBER_QC_ADAPT");
    const std::string adaptArg =
        std::string(" -a ") + (adaptEnv != nullptr ? std::string(adaptEnv) : std::string("0.0"));
    const std::string cmd = std::string(cli) + " -i " + objPath + " -u " + uvPath + " -f " +
                            std::to_string(quads) + " -s " + scaling + adaptArg + " >/dev/null 2>&1";
    const bool dbg = std::getenv("CYBER_QC_DEBUG") != nullptr;
    if (dbg) {
        std::fprintf(stderr, "[qc] edgeLen=%g area=%g quads=%ld cmd=%s\n",
                     static_cast<double>(targetEdgeLength), area, quads, cmd.c_str());
    }
    const int rc = std::system(cmd.c_str());
    if (rc == 0) {
        uv.valid = parseUvDump(uvPath, uv);
    }
    if (dbg) {
        std::fprintf(stderr, "[qc] rc=%d valid=%d isoTris=%zu\n", rc, static_cast<int>(uv.valid),
                     uv.triangles.size());
    }
    std::error_code ec;
    std::filesystem::remove(objPath, ec);
    std::filesystem::remove(uvPath, ec);
    return uv;
#endif  // CYBER_HAVE_QUADCOVER
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

// -----------------------------------------------------------------------------
// Milestone 2: isoline quad extractor.
//
// Faithful port of AutoRemesher's MIT quadextractor.cpp (Copyright (c) 2020 Jeremy
// HU, MIT license — see examples/reference/autoremesher-src). The reference's
// double-precision Vector2/Vector3, PositionKey and MeshSeparator helpers are
// reproduced here as small local types so the port stays self-contained and adds
// no library dependency. Internal geometry stays in double (the integer-isoline
// tests and ratio interpolation need it); results are narrowed to float only when
// filling IsolineQuadMesh.vertices.
// -----------------------------------------------------------------------------
namespace {

// --- double-precision vectors (mirror reference Vector2/Vector3) -------------
struct DVec2 {
    double x = 0.0;
    double y = 0.0;
    double operator[](std::size_t i) const { return i == 0 ? x : y; }
};
struct DVec3 {
    double x = 0.0;
    double y = 0.0;
    double z = 0.0;
    double operator[](std::size_t i) const { return i == 0 ? x : (i == 1 ? y : z); }
    DVec3& operator+=(const DVec3& o) {
        x += o.x;
        y += o.y;
        z += o.z;
        return *this;
    }
    DVec3& operator/=(double s) {
        x /= s;
        y /= s;
        z /= s;
        return *this;
    }
};

inline DVec2 operator*(const DVec2& v, double s) { return {v.x * s, v.y * s}; }
inline DVec2 operator+(const DVec2& a, const DVec2& b) { return {a.x + b.x, a.y + b.y}; }
inline DVec2 operator-(const DVec2& a, const DVec2& b) { return {a.x - b.x, a.y - b.y}; }
inline DVec3 operator*(const DVec3& v, double s) { return {v.x * s, v.y * s, v.z * s}; }
inline DVec3 operator+(const DVec3& a, const DVec3& b) { return {a.x + b.x, a.y + b.y, a.z + b.z}; }
inline DVec3 operator-(const DVec3& a, const DVec3& b) { return {a.x - b.x, a.y - b.y, a.z - b.z}; }

inline double dLength(const DVec3& v) { return std::sqrt(v.x * v.x + v.y * v.y + v.z * v.z); }
inline DVec3 dNormalized(const DVec3& v) {
    const double len = dLength(v);
    if (len <= std::numeric_limits<double>::epsilon()) {
        return DVec3{};
    }
    return {v.x / len, v.y / len, v.z / len};
}
inline double dDot(const DVec3& a, const DVec3& b) { return a.x * b.x + a.y * b.y + a.z * b.z; }
inline DVec3 dCross(const DVec3& a, const DVec3& b) {
    return {a.y * b.z - a.z * b.y, a.z * b.x - a.x * b.z, a.x * b.y - a.y * b.x};
}
// Unit triangle normal (reference Vector3::normal), zero vector when degenerate.
inline DVec3 dNormal(const DVec3& a, const DVec3& b, const DVec3& c) {
    return dNormalized(dCross(b - a, c - a));
}

inline bool dIsZero(double n) { return std::abs(n) <= std::numeric_limits<double>::epsilon(); }

// Reference Vector2::isInTriangle via barycentric coordinates.
inline bool dInTriangle2(const DVec2& a, const DVec2& b, const DVec2& c, const DVec2& p) {
    const DVec2 v0 = c - a;
    const DVec2 v1 = b - a;
    const DVec2 v2 = p - a;
    const double dot00 = v0.x * v0.x + v0.y * v0.y;
    const double dot01 = v0.x * v1.x + v0.y * v1.y;
    const double dot02 = v0.x * v2.x + v0.y * v2.y;
    const double dot11 = v1.x * v1.x + v1.y * v1.y;
    const double dot12 = v1.x * v2.x + v1.y * v2.y;
    const double denom = dot00 * dot11 - dot01 * dot01;
    if (dIsZero(denom)) {
        return false;
    }
    const double invDenom = 1.0 / denom;
    const double alpha = (dot11 * dot02 - dot01 * dot12) * invDenom;
    const double beta = (dot00 * dot12 - dot01 * dot02) * invDenom;
    if (alpha < 0.0 || beta < 0.0 || (1.0 - (alpha + beta)) < 0.0) {
        return false;
    }
    return true;
}

// Project 3-D points to a local 2-D frame (reference Vector3::project overload).
void dProject(const std::vector<DVec3>& in3d, std::vector<DVec2>& out2d, const DVec3& normal,
              const DVec3& axis, const DVec3& origin) {
    const DVec3 perp = dCross(normal, axis);
    out2d.clear();
    out2d.reserve(in3d.size());
    for (const DVec3& p : in3d) {
        const DVec3 dir = p - origin;
        out2d.push_back({dDot(dir, axis), dDot(dir, perp)});
    }
}

// --- 1e-5-quantised position key (reference PositionKey) ---------------------
struct PositionKey {
    long ix = 0;
    long iy = 0;
    long iz = 0;
    explicit PositionKey(const DVec3& v)
        : ix(static_cast<long>(v.x * kFactor)),
          iy(static_cast<long>(v.y * kFactor)),
          iz(static_cast<long>(v.z * kFactor)) {}
    bool operator<(const PositionKey& r) const {
        if (ix != r.ix) {
            return ix < r.ix;
        }
        if (iy != r.iy) {
            return iy < r.iy;
        }
        return iz < r.iz;
    }
    static constexpr double kFactor = 100000.0;
};

// --- mesh-island helpers (reference MeshSeparator) ---------------------------
void buildEdgeToFaceMap(const std::vector<std::vector<std::size_t>>& faces,
                        std::map<std::pair<std::size_t, std::size_t>, std::size_t>& edgeToFaceMap) {
    edgeToFaceMap.clear();
    for (std::size_t index = 0; index < faces.size(); ++index) {
        const auto& face = faces[index];
        for (std::size_t i = 0; i < face.size(); ++i) {
            const std::size_t j = (i + 1) % face.size();
            edgeToFaceMap[{face[i], face[j]}] = index;
        }
    }
}

void splitToIslands(const std::vector<std::vector<std::size_t>>& faces,
                    std::vector<std::vector<std::vector<std::size_t>>>& islands) {
    std::map<std::pair<std::size_t, std::size_t>, std::size_t> edgeToFaceMap;
    buildEdgeToFaceMap(faces, edgeToFaceMap);
    std::unordered_set<std::size_t> processed;
    std::queue<std::size_t> wait;
    for (std::size_t seed = 0; seed < faces.size(); ++seed) {
        if (processed.find(seed) != processed.end()) {
            continue;
        }
        wait.push(seed);
        std::vector<std::vector<std::size_t>> island;
        while (!wait.empty()) {
            const std::size_t index = wait.front();
            wait.pop();
            if (processed.find(index) != processed.end()) {
                continue;
            }
            const auto& face = faces[index];
            for (std::size_t i = 0; i < face.size(); ++i) {
                const std::size_t j = (i + 1) % face.size();
                const auto opp = edgeToFaceMap.find({face[j], face[i]});
                if (opp == edgeToFaceMap.end()) {
                    continue;
                }
                wait.push(opp->second);
            }
            island.push_back(faces[index]);
            processed.insert(index);
        }
        if (!island.empty()) {
            islands.push_back(island);
        }
    }
}

// --- the extractor -----------------------------------------------------------
// Mirrors AutoRemesher::QuadExtractor; member names and flow kept close to the
// reference to keep the port auditable. Cognitive complexity of extractMesh /
// extractConnections is high by nature (deeply nested orbit walk / isoline
// tracer) — this is an irreducible ported algorithm.
using Graph = std::unordered_map<std::size_t, std::unordered_set<std::size_t>>;
using EdgePair = std::pair<std::size_t, std::size_t>;

class IsolineExtractor {
public:
    IsolineExtractor(std::vector<DVec3> vertices,
                     std::vector<std::array<std::size_t, 3>> triangles,
                     std::vector<std::array<DVec2, 3>> triangleUvs)
        : m_vertices(std::move(vertices)),
          m_triangles(std::move(triangles)),
          m_triangleUvs(std::move(triangleUvs)) {}

    void extract();

    // Enable the closed-surface graph cleanup + hole fill. Correct (and required)
    // for closed inputs, corrupting for open ones — the caller gates on whether the
    // isotropic mesh has a boundary. See extract() and extractIsolineQuads().
    void setRunClosedSurfaceCleanup(bool on) { m_runClosedSurfaceCleanup = on; }

    const std::vector<DVec3>& remeshedVertices() const { return m_remeshedVertices; }
    const std::vector<std::vector<std::size_t>>& remeshedQuads() const { return m_remeshedPolygons; }

private:
    std::vector<DVec3> m_vertices;
    std::vector<std::array<std::size_t, 3>> m_triangles;
    std::vector<std::array<DVec2, 3>> m_triangleUvs;
    std::vector<DVec3> m_remeshedVertices;
    std::vector<std::vector<std::size_t>> m_remeshedPolygons;
    std::set<EdgePair> m_halfEdges;
    // Closed-surface graph cleanup + hole fill (faithfully ported below) is off by
    // default: it corrupts OPEN boundaries. See extract() and TODO(M3).
    bool m_runClosedSurfaceCleanup = false;

    void extractConnections(std::vector<DVec3>& crossPoints, std::vector<std::size_t>& sourceTriangles,
                            std::set<EdgePair>& connections);
    void extractEdges(const std::set<EdgePair>& connections, Graph& edgeConnectMap);
    void simplifyGraph(Graph& graph);
    bool collapseShortEdges(std::vector<DVec3>& crossPoints, Graph& edgeConnectMap);
    bool collapseTriangles(std::vector<DVec3>& crossPoints, Graph& edgeConnectMap);
    bool removeSingleEndpoints(Graph& edgeConnectMap);
    void collapseEdge(std::vector<DVec3>& crossPoints, Graph& edgeConnectMap, const EdgePair& edge);
    void extractMesh(std::vector<DVec3>& points, const std::vector<std::size_t>& pointSourceTriangles,
                     Graph& edgeConnectMap);
    bool testPointInTriangle(const std::vector<DVec3>& points,
                             const std::array<std::size_t, 3>& triangle,
                             const std::vector<std::size_t>& testPoints);
    void searchBoundaries(const std::set<EdgePair>& halfEdges,
                          std::vector<std::vector<std::size_t>>& loops);
    void fixHoleWithQuads(std::vector<std::size_t>& hole, bool checkScore);
    void fixHoles();
    void rebuildHalfEdges();
    bool removeIsolatedFaces();
    bool removeNonManifoldFaces();
};

void IsolineExtractor::extractEdges(const std::set<EdgePair>& connections, Graph& edgeConnectMap) {
    for (const auto& c : connections) {
        edgeConnectMap[c.first].insert(c.second);
        edgeConnectMap[c.second].insert(c.first);
    }
    simplifyGraph(edgeConnectMap);
}

void IsolineExtractor::simplifyGraph(Graph& graph) {
    for (;;) {
        std::unordered_map<std::size_t, std::pair<std::size_t, std::size_t>> delayPairs;
        for (auto it = graph.begin(); it != graph.end();) {
            if (it->second.size() != 2) {
                ++it;
                continue;
            }
            auto neighborIt = it->second.begin();
            const std::size_t firstNeighbor = *neighborIt++;
            const std::size_t secondNeighbor = *neighborIt++;
            if (delayPairs.end() != delayPairs.find(firstNeighbor) ||
                delayPairs.end() != delayPairs.find(secondNeighbor)) {
                ++it;
                continue;
            }
            delayPairs.insert({it->first, {firstNeighbor, secondNeighbor}});
            it = graph.erase(it);
        }
        if (delayPairs.empty()) {
            break;
        }
        for (const auto& dp : delayPairs) {
            graph[dp.second.first].erase(dp.first);
            graph[dp.second.first].insert(dp.second.second);
            graph[dp.second.second].erase(dp.first);
            graph[dp.second.second].insert(dp.second.first);
        }
    }
}

bool IsolineExtractor::removeSingleEndpoints(Graph& edgeConnectMap) {
    bool removed = false;
    Graph& graph = edgeConnectMap;
    std::vector<std::size_t> endpoints;
    for (const auto& node : graph) {
        if (node.second.size() == 1) {
            endpoints.push_back(node.first);
        }
    }
    for (const std::size_t endpoint : endpoints) {
        std::size_t loopIndex = endpoint;
        for (;;) {
            const auto findEndpoint = graph.find(loopIndex);
            if (findEndpoint == graph.end()) {
                break;
            }
            if (findEndpoint->second.size() != 1) {
                break;
            }
            const std::size_t neighbor = *findEndpoint->second.begin();
            graph.erase(loopIndex);
            removed = true;
            const auto findNeighbor = graph.find(neighbor);
            if (findNeighbor == graph.end()) {
                break;
            }
            findNeighbor->second.erase(loopIndex);
            loopIndex = neighbor;
        }
    }
    return removed;
}

bool IsolineExtractor::collapseTriangles(std::vector<DVec3>& crossPoints, Graph& edgeConnectMap) {
    Graph triangleEdges;
    for (const auto& level0It : edgeConnectMap) {
        const std::size_t level0 = level0It.first;
        for (const std::size_t level1 : level0It.second) {
            const auto findLevel2 = edgeConnectMap.find(level1);
            if (findLevel2 == edgeConnectMap.end()) {
                continue;
            }
            for (const std::size_t level2 : findLevel2->second) {
                if (level0 == level2) {
                    continue;
                }
                const auto findLevel3 = edgeConnectMap.find(level2);
                if (findLevel3 == edgeConnectMap.end()) {
                    continue;
                }
                if (findLevel3->second.find(level0) != findLevel3->second.end()) {
                    triangleEdges[level0].insert(level1);
                    triangleEdges[level0].insert(level2);
                    triangleEdges[level1].insert(level0);
                    triangleEdges[level1].insert(level2);
                    triangleEdges[level2].insert(level0);
                    triangleEdges[level2].insert(level1);
                }
            }
        }
    }

    if (triangleEdges.empty()) {
        return false;
    }

    std::vector<std::vector<std::size_t>> clusters;
    std::unordered_set<std::size_t> visited;
    for (const auto& edge : triangleEdges) {
        std::queue<std::size_t> q;
        q.push(edge.first);
        std::vector<std::size_t> group;
        while (!q.empty()) {
            const std::size_t v = q.front();
            q.pop();
            if (visited.find(v) != visited.end()) {
                continue;
            }
            visited.insert(v);
            group.push_back(v);
            const auto findNeighbor = triangleEdges.find(v);
            if (findNeighbor == triangleEdges.end()) {
                continue;
            }
            for (const std::size_t neighbor : findNeighbor->second) {
                if (visited.find(neighbor) == visited.end()) {
                    q.push(neighbor);
                }
            }
        }
        if (!group.empty()) {
            clusters.push_back(group);
        }
    }

    for (const auto& group : clusters) {
        DVec3 center;
        for (const std::size_t v : group) {
            center += crossPoints[v];
        }
        center /= static_cast<double>(group.size());
        crossPoints[group[0]] = center;

        std::unordered_set<std::size_t> moveNeighbors;
        for (std::size_t i = 1; i < group.size(); ++i) {
            for (const std::size_t neighbor : edgeConnectMap[group[i]]) {
                moveNeighbors.insert(neighbor);
            }
        }
        moveNeighbors.erase(group[0]);

        for (const std::size_t neighbor : moveNeighbors) {
            edgeConnectMap[group[0]].insert(neighbor);
            edgeConnectMap[neighbor].insert(group[0]);
        }
        for (const std::size_t neighbor : moveNeighbors) {
            for (std::size_t i = 1; i < group.size(); ++i) {
                edgeConnectMap[neighbor].erase(group[i]);
                if (edgeConnectMap[neighbor].empty()) {
                    edgeConnectMap.erase(neighbor);
                }
            }
        }
        for (std::size_t i = 1; i < group.size(); ++i) {
            edgeConnectMap.erase(group[i]);
            edgeConnectMap[group[0]].erase(group[i]);
        }
        if (edgeConnectMap[group[0]].empty()) {
            edgeConnectMap.erase(group[0]);
        }
    }

    return true;
}

bool IsolineExtractor::collapseShortEdges(std::vector<DVec3>& crossPoints, Graph& edgeConnectMap) {
    double totalLength = 0.0;
    std::size_t edgeCount = 0;
    std::map<EdgePair, double> edgeLengths;
    for (const auto& node : edgeConnectMap) {
        for (const std::size_t neighbor : node.second) {
            if (edgeLengths.end() != edgeLengths.find({neighbor, node.first})) {
                continue;
            }
            const double edgeLength = dLength(crossPoints[node.first] - crossPoints[neighbor]);
            totalLength += edgeLength;
            edgeLengths.insert({{node.first, neighbor}, edgeLength});
            ++edgeCount;
        }
    }
    if (edgeCount == 0) {
        return false;
    }
    const double averageEdgeLength = totalLength / static_cast<double>(edgeCount);
    const double collapsedLength = averageEdgeLength * 0.01;
    bool collapsed = false;
    for (const auto& el : edgeLengths) {
        if (el.second > collapsedLength) {
            continue;
        }
        collapseEdge(crossPoints, edgeConnectMap, el.first);
        collapsed = true;
    }
    return collapsed;
}

void IsolineExtractor::collapseEdge(std::vector<DVec3>& crossPoints, Graph& edgeConnectMap,
                                    const EdgePair& edge) {
    const auto findSecondNeighbors = edgeConnectMap.find(edge.second);
    if (findSecondNeighbors == edgeConnectMap.end()) {
        return;
    }
    const auto findFirstNeighbors = edgeConnectMap.find(edge.first);
    if (findFirstNeighbors == edgeConnectMap.end()) {
        return;
    }
    if (findSecondNeighbors->second.end() == findSecondNeighbors->second.find(edge.first)) {
        return;
    }
    if (findFirstNeighbors->second.end() == findFirstNeighbors->second.find(edge.second)) {
        return;
    }
    const auto firstNeighbors = findFirstNeighbors->second;
    crossPoints[edge.second] = (crossPoints[edge.first] + crossPoints[edge.second]) * 0.5;
    for (const std::size_t neighbor : firstNeighbors) {
        if (neighbor == edge.second) {
            continue;
        }
        edgeConnectMap[edge.second].insert(neighbor);
        edgeConnectMap[neighbor].insert(edge.second);
        edgeConnectMap[neighbor].erase(edge.first);
    }
    edgeConnectMap.erase(edge.first);
    edgeConnectMap[edge.second].erase(edge.first);
    if (edgeConnectMap[edge.second].empty()) {
        edgeConnectMap.erase(edge.second);
    }
}

void IsolineExtractor::extractMesh(std::vector<DVec3>& points,
                                   const std::vector<std::size_t>& pointSourceTriangles,
                                   Graph& edgeConnectMap) {
    std::unordered_map<std::size_t, DVec3> triangleNormals;
    for (std::size_t pointIndex = 0; pointIndex < pointSourceTriangles.size(); ++pointIndex) {
        const auto& tri = m_triangles[pointSourceTriangles[pointIndex]];
        triangleNormals.insert(
            {pointIndex, dNormal(m_vertices[tri[0]], m_vertices[tri[1]], m_vertices[tri[2]])});
    }

    auto calculateFaceNormal = [&](const std::vector<std::size_t>& corners) {
        DVec3 center;
        for (const std::size_t c : corners) {
            center += points[c];
        }
        center /= static_cast<double>(corners.size());
        DVec3 normals;
        for (std::size_t i = 0; i < corners.size(); ++i) {
            normals += dNormal(points[corners[(i + 0) % corners.size()]],
                               points[corners[(i + 1) % corners.size()]], center);
        }
        return dNormalized(normals);
    };

    auto calculateSide = [&](const std::vector<std::size_t>& corners) {
        const DVec3 ringNormal = calculateFaceNormal(corners);
        DVec3 originalNormal;
        for (const std::size_t c : corners) {
            originalNormal += triangleNormals[c];
        }
        const double d = dDot(ringNormal, dNormalized(originalNormal));
        const double dotThreshold = 0.259;  // > 75 or < 105 degrees
        if (d > dotThreshold) {
            return 1;
        }
        if (d < -dotThreshold) {
            return -1;
        }
        return 0;
    };

    std::set<std::tuple<std::size_t, std::size_t, std::size_t>> corners;
    auto& halfEdges = m_halfEdges;
    auto isCornerUsed = [&](std::size_t previous, std::size_t current, std::size_t next) {
        if (corners.end() != corners.find(std::make_tuple(previous, current, next))) {
            return true;
        }
        return corners.end() != corners.find(std::make_tuple(next, current, previous));
    };
    auto isFaceCornerExist = [&](const std::vector<std::size_t>& vertices) {
        for (std::size_t i = 0; i < vertices.size(); ++i) {
            const std::size_t j = (i + 1) % vertices.size();
            const std::size_t k = (i + 2) % vertices.size();
            if (isCornerUsed(vertices[i], vertices[j], vertices[k])) {
                return true;
            }
        }
        return false;
    };
    auto addFaceCorners = [&](const std::vector<std::size_t>& vertices) {
        for (std::size_t i = 0; i < vertices.size(); ++i) {
            const std::size_t j = (i + 1) % vertices.size();
            const std::size_t k = (i + 2) % vertices.size();
            corners.insert(std::make_tuple(vertices[i], vertices[j], vertices[k]));
            corners.insert(std::make_tuple(vertices[k], vertices[j], vertices[i]));
        }
    };
    auto isFaceHalfEdgeExist = [&](const std::vector<std::size_t>& vertices) {
        for (std::size_t i = 0; i < vertices.size(); ++i) {
            const std::size_t j = (i + 1) % vertices.size();
            if (halfEdges.end() != halfEdges.find({vertices[i], vertices[j]})) {
                return true;
            }
        }
        return false;
    };
    auto addFaceHalfEdges = [&](const std::vector<std::size_t>& vertices) {
        for (std::size_t i = 0; i < vertices.size(); ++i) {
            const std::size_t j = (i + 1) % vertices.size();
            halfEdges.insert({vertices[i], vertices[j]});
        }
    };
    auto emitFace = [&](std::vector<std::size_t> forward) {
        if (isFaceCornerExist(forward)) {
            return;
        }
        const int side = calculateSide(forward);
        if (side > 0) {
            if (!isFaceHalfEdgeExist(forward)) {
                addFaceCorners(forward);
                addFaceHalfEdges(forward);
                m_remeshedPolygons.push_back(std::move(forward));
            }
        } else if (side < 0) {
            std::vector<std::size_t> reversed(forward.rbegin(), forward.rend());
            if (!isFaceHalfEdgeExist(reversed)) {
                addFaceCorners(reversed);
                addFaceHalfEdges(reversed);
                m_remeshedPolygons.push_back(std::move(reversed));
            }
        }
    };

    const std::size_t triangleRound = 4;
    for (std::size_t round = 0; round < 5; ++round) {
        for (const auto& level0It : edgeConnectMap) {
            const std::size_t level0 = level0It.first;
            const auto findLevel1 = edgeConnectMap.find(level0);
            if (findLevel1 == edgeConnectMap.end()) {
                continue;
            }
            for (const std::size_t level1 : findLevel1->second) {
                const auto findLevel2 = edgeConnectMap.find(level1);
                if (findLevel2 == edgeConnectMap.end()) {
                    continue;
                }
                if (halfEdges.find({level0, level1}) != halfEdges.end() &&
                    halfEdges.find({level1, level0}) != halfEdges.end()) {
                    continue;
                }
                for (const std::size_t level2 : findLevel2->second) {
                    if (level0 == level2) {
                        continue;
                    }
                    const auto findLevel3 = edgeConnectMap.find(level2);
                    if (findLevel3 == edgeConnectMap.end()) {
                        continue;
                    }
                    if (halfEdges.find({level1, level2}) != halfEdges.end() &&
                        halfEdges.find({level2, level1}) != halfEdges.end()) {
                        continue;
                    }
                    for (const std::size_t level3 : findLevel3->second) {
                        if (level0 == level3) {
                            if (triangleRound == round) {
                                emitFace({level0, level1, level2});
                                break;
                            }
                        } else {
                            if (triangleRound == round) {
                                break;
                            }
                        }
                        if (level1 == level3 || level0 == level3) {
                            continue;
                        }
                        const auto findLevel4 = edgeConnectMap.find(level3);
                        if (findLevel4 == edgeConnectMap.end()) {
                            continue;
                        }
                        if (halfEdges.find({level2, level3}) != halfEdges.end() &&
                            halfEdges.find({level3, level2}) != halfEdges.end()) {
                            continue;
                        }
                        for (const std::size_t level4 : findLevel4->second) {
                            if (level0 != level4) {
                                if (level2 == level4 || level1 == level4) {
                                    continue;
                                }
                                if (round < 1) {
                                    continue;
                                }
                                const auto findLevel5 = edgeConnectMap.find(level4);
                                if (findLevel5 == edgeConnectMap.end()) {
                                    continue;
                                }
                                if (halfEdges.find({level3, level4}) != halfEdges.end() &&
                                    halfEdges.find({level4, level3}) != halfEdges.end()) {
                                    continue;
                                }
                                for (const std::size_t level5 : findLevel5->second) {
                                    if (level0 != level5) {
                                        if (level3 == level5 || level2 == level5 || level1 == level5) {
                                            continue;
                                        }
                                        if (round < 2) {
                                            continue;
                                        }
                                        const auto findLevel6 = edgeConnectMap.find(level5);
                                        if (findLevel6 == edgeConnectMap.end()) {
                                            continue;
                                        }
                                        if (halfEdges.find({level4, level5}) != halfEdges.end() &&
                                            halfEdges.find({level5, level4}) != halfEdges.end()) {
                                            continue;
                                        }
                                        for (const std::size_t level6 : findLevel6->second) {
                                            if (level0 != level6) {
                                                if (level4 == level6 || level3 == level6 ||
                                                    level2 == level6 || level1 == level6) {
                                                    continue;
                                                }
                                                if (round < 3) {
                                                    continue;
                                                }
                                                const auto findLevel7 = edgeConnectMap.find(level6);
                                                if (findLevel7 == edgeConnectMap.end()) {
                                                    continue;
                                                }
                                                if (halfEdges.find({level5, level6}) != halfEdges.end() &&
                                                    halfEdges.find({level6, level5}) != halfEdges.end()) {
                                                    continue;
                                                }
                                                for (const std::size_t level7 : findLevel7->second) {
                                                    if (level0 != level7) {
                                                        continue;
                                                    }
                                                    if (3 != round) {
                                                        break;
                                                    }
                                                    emitFace({level0, level1, level2, level3, level4,
                                                              level5, level6});
                                                    break;
                                                }
                                                continue;
                                            }
                                            if (2 != round) {
                                                break;
                                            }
                                            emitFace({level0, level1, level2, level3, level4, level5});
                                            break;
                                        }
                                        continue;
                                    }
                                    if (1 != round) {
                                        break;
                                    }
                                    emitFace({level0, level1, level2, level3, level4});
                                    break;
                                }
                                continue;
                            }
                            if (0 != round) {
                                break;
                            }
                            emitFace({level0, level1, level2, level3});
                            break;
                        }
                    }
                }
            }
        }
    }

    m_remeshedVertices = points;
}

void IsolineExtractor::extractConnections(std::vector<DVec3>& crossPoints,
                                          std::vector<std::size_t>& sourceTriangles,
                                          std::set<EdgePair>& connections) {
    std::map<PositionKey, std::size_t> crossPointMap;

    auto addCrossPoint = [&](const DVec3& position3, std::size_t triangleIndex) {
        const auto insertResult = crossPointMap.insert({PositionKey(position3), crossPoints.size()});
        if (insertResult.second) {
            crossPoints.push_back(position3);
            sourceTriangles.push_back(triangleIndex);
        }
        return insertResult.first->second;
    };
    auto addConnection = [&](std::size_t fromPointIndex, std::size_t toPointIndex) {
        if (fromPointIndex != toPointIndex) {
            connections.insert({fromPointIndex, toPointIndex});
        }
    };

    struct CrossPoint {
        DVec3 position3;
        DVec2 position2;
        int integer = 0;
    };

    for (std::size_t triangleIndex = 0; triangleIndex < m_triangles.size(); ++triangleIndex) {
        const auto& cornerUvs = m_triangleUvs[triangleIndex];
        const auto& cornerIndices = m_triangles[triangleIndex];

        // Extract intersections of isolines with edges.
        std::map<int, std::vector<std::vector<CrossPoint>>> lines[2];
        bool edgeCollapsed[2][3] = {{false, false, false}, {false, false, false}};
        for (std::size_t i = 0; i < 2; ++i) {
            for (std::size_t j = 0; j < 3; ++j) {
                const std::size_t k = (j + 1) % 3;
                const DVec2& current = cornerUvs[j];
                const DVec2& next = cornerUvs[k];
                if (dIsZero(static_cast<double>(static_cast<int>(current[i])) - current[i]) &&
                    dIsZero(current[i] - next[i])) {
                    const int integer = static_cast<int>(current[i]);
                    edgeCollapsed[i][j] = true;
                    CrossPoint fromPoint;
                    fromPoint.position3 = m_vertices[cornerIndices[j]];
                    fromPoint.position2 = cornerUvs[j];
                    fromPoint.integer = integer;
                    CrossPoint toPoint;
                    toPoint.position3 = m_vertices[cornerIndices[k]];
                    toPoint.position2 = cornerUvs[k];
                    toPoint.integer = integer;
                    lines[i][integer].push_back({fromPoint, toPoint});
                }
            }
            std::map<int, std::vector<CrossPoint>> points;
            for (std::size_t j = 0; j < 3; ++j) {
                const std::size_t k = (j + 1) % 3;
                const DVec2& current = cornerUvs[j];
                const DVec2& next = cornerUvs[k];
                const double distance = std::abs(current[i] - next[i]);
                if (static_cast<int>(current[i]) != static_cast<int>(next[i]) ||
                    (current[i] > 0) != (next[i] > 0)) {
                    int lowInteger = 0;
                    int highInteger = 0;
                    double fromPosition = 0.0;
                    std::size_t fromIndex = 0;
                    std::size_t toIndex = 0;
                    if (current[i] < next[i]) {
                        lowInteger = static_cast<int>(current[i]);
                        highInteger = static_cast<int>(next[i]);
                        fromPosition = current[i];
                        fromIndex = j;
                        toIndex = k;
                    } else {
                        lowInteger = static_cast<int>(next[i]);
                        highInteger = static_cast<int>(current[i]);
                        fromPosition = next[i];
                        fromIndex = k;
                        toIndex = j;
                    }
                    for (int integer = lowInteger; integer <= highInteger; ++integer) {
                        const double ratio = (static_cast<double>(integer) - fromPosition) / distance;
                        if (ratio < 0 || ratio > 1) {
                            continue;
                        }
                        if (dIsZero(ratio) || dIsZero(ratio - 1.0)) {
                            if (edgeCollapsed[i][j]) {
                                continue;
                            }
                        }
                        CrossPoint point;
                        point.position3 = m_vertices[cornerIndices[fromIndex]] * (1 - ratio) +
                                          m_vertices[cornerIndices[toIndex]] * ratio;
                        point.position2 =
                            cornerUvs[fromIndex] * (1 - ratio) + cornerUvs[toIndex] * ratio;
                        point.integer = integer;
                        points[integer].push_back(point);
                    }
                }
            }
            for (const auto& pt : points) {
                for (std::size_t pointIndex = 0; pointIndex < pt.second.size(); ++pointIndex) {
                    const std::size_t nextPointIndex = (pointIndex + 1) % pt.second.size();
                    lines[i][pt.first].push_back({pt.second[pointIndex], pt.second[nextPointIndex]});
                }
            }
        }

        // Segment lines by the perpendicular isolines so each crossing splits a segment.
        for (std::size_t i = 0; i < 2; ++i) {
            const std::size_t j = (i + 1) % 2;
            for (const auto& targetIt : lines[i]) {
                for (const auto& target : targetIt.second) {
                    std::vector<std::vector<CrossPoint>> segments = {target};
                    for (const auto& splitIt : lines[j]) {
                        const std::size_t coordIndex = j;
                        const double segmentPosition =
                            splitIt.second.front().front().position2[coordIndex];
                        for (int segmentIndex = static_cast<int>(segments.size()) - 1;
                             segmentIndex >= 0; --segmentIndex) {
                            auto& segment = segments[static_cast<std::size_t>(segmentIndex)];
                            const DVec2& uv0 = segment[0].position2;
                            const DVec2& uv1 = segment[1].position2;
                            const double distance = std::abs(uv0[coordIndex] - uv1[coordIndex]);
                            if (dIsZero(distance)) {
                                continue;
                            }
                            double fromPosition = 0.0;
                            double toPosition = 0.0;
                            std::size_t fromIndex = 0;
                            std::size_t toIndex = 0;
                            if (uv0[coordIndex] < uv1[coordIndex]) {
                                fromPosition = uv0[coordIndex];
                                toPosition = uv1[coordIndex];
                                fromIndex = 0;
                                toIndex = 1;
                            } else {
                                fromPosition = uv1[coordIndex];
                                toPosition = uv0[coordIndex];
                                fromIndex = 1;
                                toIndex = 0;
                            }
                            if (segmentPosition < fromPosition || segmentPosition > toPosition) {
                                continue;
                            }
                            const double ratio = (segmentPosition - fromPosition) / distance;
                            const DVec3 position3 = segment[fromIndex].position3 * (1 - ratio) +
                                                    segment[toIndex].position3 * ratio;
                            const DVec2 position2 = segment[fromIndex].position2 * (1 - ratio) +
                                                    segment[toIndex].position2 * ratio;
                            const int integer = segment[toIndex].integer;
                            CrossPoint newFromPoint;
                            newFromPoint.position3 = position3;
                            newFromPoint.position2 = position2;
                            newFromPoint.integer = integer;
                            const CrossPoint newToPoint = segment[toIndex];
                            segment[toIndex] = newFromPoint;
                            segments.push_back({newFromPoint, newToPoint});
                        }
                    }
                    for (const auto& segment : segments) {
                        addConnection(addCrossPoint(segment[0].position3, triangleIndex),
                                      addCrossPoint(segment[1].position3, triangleIndex));
                    }
                }
            }
        }
    }
}

bool IsolineExtractor::testPointInTriangle(const std::vector<DVec3>& points,
                                           const std::array<std::size_t, 3>& triangle,
                                           const std::vector<std::size_t>& testPoints) {
    const DVec3 triangleNormal = dNormal(points[triangle[0]], points[triangle[1]], points[triangle[2]]);
    std::vector<DVec3> pointsIn3d;
    for (const std::size_t it : triangle) {
        pointsIn3d.push_back(points[it]);
    }
    for (const std::size_t it : testPoints) {
        pointsIn3d.push_back(points[it]);
    }
    std::vector<DVec2> pointsIn2d;
    const DVec3 origin =
        (points[triangle[0]] + points[triangle[1]] + points[triangle[2]]) * (1.0 / 3.0);
    const DVec3 axis = dNormalized(points[triangle[0]] - origin);
    dProject(pointsIn3d, pointsIn2d, triangleNormal, axis, origin);
    const DVec2& a = pointsIn2d[0];
    const DVec2& b = pointsIn2d[1];
    const DVec2& c = pointsIn2d[2];
    for (std::size_t i = 3; i < pointsIn2d.size(); ++i) {
        if (dInTriangle2(a, b, c, pointsIn2d[i])) {
            return true;
        }
    }
    return false;
}

void IsolineExtractor::rebuildHalfEdges() {
    m_halfEdges.clear();
    for (const auto& face : m_remeshedPolygons) {
        for (std::size_t i = 0; i < face.size(); ++i) {
            const std::size_t j = (i + 1) % face.size();
            m_halfEdges.insert({face[i], face[j]});
        }
    }
}

void IsolineExtractor::fixHoles() {
    std::vector<std::vector<std::size_t>> loops;
    searchBoundaries(m_halfEdges, loops);
    for (auto& loop : loops) {
        if (loop.size() > 65) {
            continue;
        }
        fixHoleWithQuads(loop, true);
        if (loop.size() >= 4) {
            fixHoleWithQuads(loop, false);
        }
    }
}

void IsolineExtractor::fixHoleWithQuads(std::vector<std::size_t>& hole, bool checkScore) {
    auto recordHalfEdgesOfLastPolygon = [&]() {
        const auto& face = m_remeshedPolygons.back();
        for (std::size_t i = 0; i < face.size(); ++i) {
            const std::size_t j = (i + 1) % face.size();
            m_halfEdges.insert({face[i], face[j]});
        }
    };

    for (;;) {
        if (hole.size() <= 2) {
            return;
        }
        if (3 == hole.size()) {
            m_remeshedPolygons.push_back({hole[2], hole[1], hole[0]});
            recordHalfEdgesOfLastPolygon();
            return;
        }
        if (4 == hole.size()) {
            m_remeshedPolygons.push_back({hole[3], hole[2], hole[1], hole[0]});
            recordHalfEdgesOfLastPolygon();
            return;
        }

        const int n = static_cast<int>(hole.size());
        std::vector<std::pair<int, double>> edgeScores;
        edgeScores.reserve(hole.size());
        for (int i = 0; i < n; ++i) {
            const int h = (i + n - 1) % n;
            const int j = (i + 1) % n;
            const int k = (j + 1) % n;
            const DVec3 left = dNormalized(m_remeshedVertices[hole[static_cast<std::size_t>(h)]] -
                                           m_remeshedVertices[hole[static_cast<std::size_t>(i)]]);
            const DVec3 right = dNormalized(m_remeshedVertices[hole[static_cast<std::size_t>(k)]] -
                                            m_remeshedVertices[hole[static_cast<std::size_t>(j)]]);
            edgeScores.push_back({i, dDot(left, right)});
        }
        std::sort(edgeScores.begin(), edgeScores.end(),
                  [](const std::pair<int, double>& a, const std::pair<int, double>& b) {
                      return a.second < b.second;
                  });
        bool holeChanged = false;
        for (int edgeIndex = static_cast<int>(edgeScores.size()) - 1; edgeIndex >= 0; --edgeIndex) {
            const auto& score = edgeScores[static_cast<std::size_t>(edgeIndex)];
            if (checkScore && score.second <= 0) {
                return;
            }
            const int i = score.first;
            const int h = (i + n - 1) % n;
            const int j = (i + 1) % n;
            const int k = (j + 1) % n;
            std::vector<std::size_t> candidate = {hole[static_cast<std::size_t>(k)],
                                                  hole[static_cast<std::size_t>(j)],
                                                  hole[static_cast<std::size_t>(i)],
                                                  hole[static_cast<std::size_t>(h)]};
            if (m_halfEdges.end() != m_halfEdges.find({candidate[0], candidate[1]}) ||
                m_halfEdges.end() != m_halfEdges.find({candidate[1], candidate[2]}) ||
                m_halfEdges.end() != m_halfEdges.find({candidate[2], candidate[3]}) ||
                m_halfEdges.end() != m_halfEdges.find({candidate[3], candidate[0]})) {
                continue;
            }
            std::vector<std::size_t> remainPoints;
            for (int w = 0; w < n; ++w) {
                if (w == i || w == j || w == h || w == k) {
                    continue;
                }
                remainPoints.push_back(hole[static_cast<std::size_t>(w)]);
            }
            if (testPointInTriangle(m_remeshedVertices, {candidate[0], candidate[1], candidate[2]},
                                    remainPoints) ||
                testPointInTriangle(m_remeshedVertices, {candidate[2], candidate[3], candidate[0]},
                                    remainPoints)) {
                continue;
            }
            m_remeshedPolygons.push_back(candidate);
            recordHalfEdgesOfLastPolygon();
            std::vector<std::size_t> newHole;
            for (int w = 0; w < n; ++w) {
                if (w == i || w == j) {
                    continue;
                }
                newHole.push_back(hole[static_cast<std::size_t>(w)]);
            }
            hole = newHole;
            holeChanged = true;
            break;
        }
        if (!holeChanged) {
            break;
        }
    }
}

void IsolineExtractor::searchBoundaries(const std::set<EdgePair>& halfEdges,
                                        std::vector<std::vector<std::size_t>>& loops) {
    Graph nextMap;
    for (const auto& he : halfEdges) {
        if (halfEdges.end() != halfEdges.find({he.second, he.first})) {
            continue;
        }
        nextMap[he.first].insert(he.second);
    }

    while (!nextMap.empty()) {
        auto it = nextMap.begin();
        std::vector<std::size_t> loop;
        const std::size_t startVertex = it->first;
        bool valid = false;
        while (it != nextMap.end()) {
            if (startVertex == it->first && loop.size() >= 3) {
                valid = true;
                break;
            }
            loop.push_back(it->first);
            if (it->second.size() != 1) {
                break;
            }
            it = nextMap.find(*it->second.begin());
        }
        for (const std::size_t v : loop) {
            nextMap.erase(v);
        }
        if (valid) {
            loops.push_back(loop);
        }
    }
}

bool IsolineExtractor::removeIsolatedFaces() {
    std::vector<std::vector<std::vector<std::size_t>>> quadsIslands;
    splitToIslands(m_remeshedPolygons, quadsIslands);
    if (quadsIslands.empty()) {
        return false;
    }
    m_remeshedPolygons = *std::max_element(
        quadsIslands.begin(), quadsIslands.end(),
        [](const std::vector<std::vector<std::size_t>>& a,
           const std::vector<std::vector<std::size_t>>& b) { return a.size() < b.size(); });
    return true;
}

bool IsolineExtractor::removeNonManifoldFaces() {
    bool changed = false;
    std::map<EdgePair, std::size_t> edgeToFaceMap;
    buildEdgeToFaceMap(m_remeshedPolygons, edgeToFaceMap);
    std::unordered_map<std::size_t, std::size_t> vertexOpenBoundaryCountMap;
    for (const auto& e : edgeToFaceMap) {
        if (edgeToFaceMap.end() != edgeToFaceMap.find({e.first.second, e.first.first})) {
            continue;
        }
        vertexOpenBoundaryCountMap[e.first.first]++;
        vertexOpenBoundaryCountMap[e.first.second]++;
    }
    std::vector<std::vector<std::size_t>> manifoldFaces;
    for (const auto& face : m_remeshedPolygons) {
        bool isNonManifold = false;
        for (const std::size_t v : face) {
            const auto findCount = vertexOpenBoundaryCountMap.find(v);
            if (findCount == vertexOpenBoundaryCountMap.end()) {
                continue;
            }
            if (findCount->second > 2) {
                isNonManifold = true;
                break;
            }
        }
        if (isNonManifold) {
            changed = true;
            continue;
        }
        manifoldFaces.push_back(face);
    }
    m_remeshedPolygons = manifoldFaces;
    return changed;
}

void IsolineExtractor::extract() {
    std::vector<DVec3> crossPoints;
    std::vector<std::size_t> crossPointSourceTriangles;
    std::set<EdgePair> connections;
    extractConnections(crossPoints, crossPointSourceTriangles, connections);

    // M2 tracer core: raw connection adjacency -> orbit-walk into oriented quads.
    // For a seamless integer grid this is exact — every crossing is a transversal
    // (valence-4) node — so the raw graph already yields a clean quad mesh.
    //
    // The reference additionally runs a closed-surface graph-cleanup stage here
    //   extractEdges (simplifyGraph) -> collapseShortEdges -> collapseTriangles
    //   -> removeSingleEndpoints  [and, after extractMesh]  fixHoles ->
    //   removeIsolatedFaces / removeNonManifoldFaces.
    // Those steps merge redundant mid-isoline sample points and patch small holes
    // on a WATERTIGHT input. They are faithfully ported below but intentionally NOT
    // wired into this path yet: on an OPEN surface they corrupt the boundary
    // (simplifyGraph deletes genuine valence-2 boundary corners; fixHoles fills the
    // outer boundary loop as if it were a hole). Enabling them for dense/curved
    // seamless UVs is Milestone 3 and needs boundary-aware gating (e.g. a turn-angle
    // guard on simplifyGraph and outer-boundary detection in fixHoles).
    // TODO(M3): wire the cleanup stage with open-boundary safety.
    Graph edgeConnectMap;
    if (m_runClosedSurfaceCleanup) {
        extractEdges(connections, edgeConnectMap);
        if (collapseShortEdges(crossPoints, edgeConnectMap)) {
            simplifyGraph(edgeConnectMap);
        }
        collapseTriangles(crossPoints, edgeConnectMap);
        if (removeSingleEndpoints(edgeConnectMap)) {
            simplifyGraph(edgeConnectMap);
        }
    } else {
        for (const auto& c : connections) {
            edgeConnectMap[c.first].insert(c.second);
            edgeConnectMap[c.second].insert(c.first);
        }
    }

    extractMesh(crossPoints, crossPointSourceTriangles, edgeConnectMap);

    if (m_runClosedSurfaceCleanup) {
        fixHoles();
        bool changed = false;
        if (removeIsolatedFaces()) {
            changed = true;
        }
        while (removeNonManifoldFaces()) {
            changed = true;
            removeIsolatedFaces();
        }
        if (changed) {
            rebuildHalfEdges();
            fixHoles();
        }
    }

    // Compact: keep only referenced vertices, in ascending original order.
    std::set<std::size_t> usedVertices;
    for (const auto& face : m_remeshedPolygons) {
        for (const std::size_t v : face) {
            usedVertices.insert(v);
        }
    }
    if (usedVertices.size() < m_remeshedVertices.size()) {
        std::vector<DVec3> compactedVertices;
        std::unordered_map<std::size_t, std::size_t> oldToNew;
        compactedVertices.reserve(usedVertices.size());
        for (const std::size_t oldIndex : usedVertices) {
            oldToNew[oldIndex] = compactedVertices.size();
            compactedVertices.push_back(m_remeshedVertices[oldIndex]);
        }
        for (auto& face : m_remeshedPolygons) {
            for (auto& v : face) {
                v = oldToNew[v];
            }
        }
        m_remeshedVertices = std::move(compactedVertices);
    }
}

}  // namespace

// Milestone 2 entry point: trace the seamless UV's integer isolines into an
// oriented quad mesh. Returns empty for an invalid/empty UV.
// Fraction of undirected edges that are on a boundary (incident to a single
// triangle). ~0 for a closed surface (or a closed surface with a few numerical
// cracks); large for a genuine open disk, whose whole perimeter is boundary.
double boundaryEdgeFraction(const std::vector<std::array<Index, 3>>& triangles) {
    if (triangles.empty()) {
        return 1.0;
    }
    std::unordered_map<std::uint64_t, int> edgeCount;
    edgeCount.reserve(triangles.size() * 3);
    const auto key = [](Index a, Index b) {
        const std::uint64_t lo = static_cast<std::uint64_t>(std::min(a, b));
        const std::uint64_t hi = static_cast<std::uint64_t>(std::max(a, b));
        return (hi << 32) | lo;
    };
    for (const auto& t : triangles) {
        ++edgeCount[key(t[0], t[1])];
        ++edgeCount[key(t[1], t[2])];
        ++edgeCount[key(t[2], t[0])];
    }
    std::size_t boundary = 0;
    for (const auto& [e, count] : edgeCount) {
        (void)e;
        if (count == 1) {
            ++boundary;
        }
    }
    return static_cast<double>(boundary) / static_cast<double>(edgeCount.size());
}

IsolineQuadMesh extractIsolineQuads(const Mesh& /*mesh*/, const SeamlessUv& uv) {
    if (!uv.valid || uv.triangles.empty()) {
        return IsolineQuadMesh{};
    }

    std::vector<DVec3> vertices;
    vertices.reserve(uv.vertices.size());
    for (const Vec3& p : uv.vertices) {
        vertices.push_back({static_cast<double>(p.x), static_cast<double>(p.y),
                            static_cast<double>(p.z)});
    }
    std::vector<std::array<std::size_t, 3>> triangles;
    triangles.reserve(uv.triangles.size());
    for (const auto& t : uv.triangles) {
        triangles.push_back({static_cast<std::size_t>(t[0]), static_cast<std::size_t>(t[1]),
                             static_cast<std::size_t>(t[2])});
    }
    std::vector<std::array<DVec2, 3>> triangleUvs;
    triangleUvs.reserve(uv.triangleUv.size());
    for (const auto& t : uv.triangleUv) {
        triangleUvs.push_back({DVec2{static_cast<double>(t[0].x), static_cast<double>(t[0].y)},
                               DVec2{static_cast<double>(t[1].x), static_cast<double>(t[1].y)},
                               DVec2{static_cast<double>(t[2].x), static_cast<double>(t[2].y)}});
    }

    // Boundary-aware gate: the closed-surface graph cleanup (simplifyGraph / fixHoles /
    // collapse*) merges the raw isoline oversampling into clean quad cells and is REQUIRED
    // on a closed (or nearly-closed) surface — a sphere goes from 357 non-quad n-gons to
    // 1.1% irregular with it on — but CORRUPTS a genuine open disk (it fills the perimeter
    // as a hole and deletes real boundary corners). A binary closed test is too brittle:
    // the harness re-remeshes and leaves a few numerical crack edges, so a genuinely closed
    // model (e.g. spot) reads as "open" and gets the wrong branch. Decide on the boundary
    // FRACTION instead: ~0 for a closed/cracked surface, large (whole perimeter) for a real
    // open disk. Below 10% we treat the surface as closed and run the cleanup.
    const double boundaryFrac = boundaryEdgeFraction(uv.triangles);
    const bool closed = boundaryFrac < 0.10;

    IsolineExtractor extractor(std::move(vertices), std::move(triangles), std::move(triangleUvs));
    extractor.setRunClosedSurfaceCleanup(closed);
    extractor.extract();
    if (std::getenv("CYBER_QC_DEBUG") != nullptr) {
        std::fprintf(stderr, "[qc] extract closed=%d bndFrac=%.3f uvTris=%zu -> quads=%zu verts=%zu\n",
                     static_cast<int>(closed), boundaryFrac, uv.triangles.size(),
                     extractor.remeshedQuads().size(), extractor.remeshedVertices().size());
    }

    IsolineQuadMesh out;
    out.vertices.reserve(extractor.remeshedVertices().size());
    for (const DVec3& v : extractor.remeshedVertices()) {
        out.vertices.push_back({static_cast<float>(v.x), static_cast<float>(v.y),
                                static_cast<float>(v.z)});
    }
    out.quads = extractor.remeshedQuads();
    return out;
}

namespace {

// Quad count implied by a target edge length: surface area / edge^2. Used to
// calibrate the harness scaling so the extracted count tracks the request.
double meshTargetQuads(const Mesh& mesh, float targetEdgeLength) {
    if (targetEdgeLength <= 0.0f) {
        return 0.0;
    }
    std::vector<Vec3> pos;
    std::vector<std::vector<Index>> faces;
    mesh.toIndexed(pos, faces);
    double area = 0.0;
    for (const auto& fc : faces) {
        for (std::size_t k = 1; k + 1 < fc.size(); ++k) {
            area += 0.5 * static_cast<double>(length(cross(pos[fc[k]] - pos[fc[0]],
                                                           pos[fc[k + 1]] - pos[fc[0]])));
        }
    }
    const double cell = static_cast<double>(targetEdgeLength) * static_cast<double>(targetEdgeLength);
    return area / cell;
}

// IQuadrangulator implementation (Milestone 3). Runs the full QuadCover pipeline:
// seamless integer-grid UV (M1, out-of-process via CYBER_QUADCOVER_CLI) -> isoline
// trace + boundary-aware cleanup (M2) -> replace `mesh` in place with the quad mesh.
// Every failure path leaves `mesh` untouched, so the pipeline degrades cleanly (e.g.
// when the harness binary is absent, computeSeamlessUv returns invalid and we report
// a reason without corrupting the input triangle island).
class QuadCoverQuadrangulator final : public IQuadrangulator {
public:
    explicit QuadCoverQuadrangulator(int fieldIterations) : m_fieldIterations(fieldIterations) {}

    Outcome quadrangulate(Mesh& mesh, float targetEdgeLength, ProgressSink* progress,
                          const CancelToken* cancel) override {
        if (cancel != nullptr && cancel->isCancelled()) {
            return {.success = false, .cancelled = true, .failureReason = {}};
        }
        if (progress != nullptr) {
            progress->report(0.0f, "quadrangulate (quad-cover: seamless UV)");
        }
        (void)m_fieldIterations;

        // Target quad count implied by the requested edge length. The harness scaling only
        // loosely maps to the extracted count, so hit the target with a short closed loop:
        // extract, measure, correct the scaling by sqrt(actual/target) (quads ~ 1/scaling^2),
        // and re-solve once if we are off by more than 25%.
        const double targetQuads = meshTargetQuads(mesh, targetEdgeLength);
        IsolineQuadMesh out;
        float scaling = 0.5f;
        for (int attempt = 0; attempt < 2; ++attempt) {
            const SeamlessUv uv = computeSeamlessUv(mesh, targetEdgeLength, scaling);
            if (!uv.valid) {
                return {.success = false, .cancelled = false,
                        .failureReason = "quad-cover seamless UV unavailable "
                                         "(set CYBER_QUADCOVER_CLI to the autoremesher_cli build)"};
            }
            if (cancel != nullptr && cancel->isCancelled()) {
                return {.success = false, .cancelled = true, .failureReason = {}};
            }
            out = extractIsolineQuads(mesh, uv);
            const double got = static_cast<double>(out.quads.size());
            if (std::getenv("CYBER_QC_SCALING") != nullptr || targetQuads <= 0.0 || got <= 0.0) {
                break;  // fixed scaling (experiment) or nothing to calibrate against
            }
            const double ratio = got / targetQuads;
            if (ratio > 0.75 && ratio < 1.33) {
                break;  // within 25% -> accept
            }
            scaling = std::clamp(scaling * static_cast<float>(std::sqrt(ratio)), 0.2f, 1.5f);
        }
        if (progress != nullptr) {
            progress->report(0.5f, "quadrangulate (quad-cover: isoline extract)");
        }
        if (out.quads.empty()) {
            return {.success = false, .cancelled = false,
                    .failureReason = "quad-cover isoline extraction produced no faces"};
        }

        std::vector<std::vector<Index>> faces;
        faces.reserve(out.quads.size());
        for (const auto& q : out.quads) {
            std::vector<Index> f;
            f.reserve(q.size());
            for (const std::size_t v : q) {
                f.push_back(static_cast<Index>(v));
            }
            faces.push_back(std::move(f));
        }
        Mesh quads = Mesh::fromIndexed(out.vertices, faces);
        if (quads.faceCount() == 0) {
            return {.success = false, .cancelled = false,
                    .failureReason = "quad-cover extraction yielded a degenerate mesh"};
        }
        mesh = std::move(quads);
        if (progress != nullptr) {
            progress->report(1.0f, "quadrangulate");
        }
        return {.success = true, .cancelled = false, .failureReason = {}};
    }

    [[nodiscard]] std::string name() const override { return "quad-cover"; }

private:
    int m_fieldIterations;
};

}  // namespace

std::unique_ptr<IQuadrangulator> makeQuadCoverQuadrangulator(int fieldIterations) {
    return std::make_unique<QuadCoverQuadrangulator>(fieldIterations);
}

bool quadCoverAvailable() {
#ifdef CYBER_HAVE_QUADCOVER
    return true;  // in-process solver linked
#else
    return std::getenv("CYBER_QUADCOVER_CLI") != nullptr;  // out-of-process harness
#endif
}

}  // namespace cyber::remesh
