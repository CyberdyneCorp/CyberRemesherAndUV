// In-process QuadCover seamless-UV solve. See autoremesher_solve.hpp.
//
// Mirrors the sequence autoremesher_harness.cpp's main() performs on the
// AutoRemesher public API, minus the OBJ/UV file I/O and the std::_Exit(0)
// shortcut (a library cannot terminate the process). GEO::initialize() is guarded
// so it runs once even across multiple islands/threads, and GEO::terminate() is
// never called — Geogram's teardown is what the harness sidestepped with _Exit,
// so we simply leave the runtime initialized for the life of the process.
#include "autoremesher_solve.hpp"

#include <mutex>

#include <geogram/basic/common.h>

#include <AutoRemesher/AutoRemesher>
#include <AutoRemesher/Vector2>
#include <AutoRemesher/Vector3>

namespace cyber::remesh::qcsolver {

namespace {

// GEO::initialize() must run before any Geogram use and exactly once per process.
// std::call_once makes that safe under concurrent island solves; we never pair it
// with GEO::terminate() (that static teardown is the double-free the harness
// avoided via std::_Exit — leaving the runtime up for the process lifetime is the
// library-safe equivalent).
void ensureGeogramInitialized() {
    static std::once_flag once;
    std::call_once(once, [] { GEO::initialize(); });
}

}  // namespace

SeamlessSolveResult solveSeamlessUv(const std::vector<std::array<double, 3>>& verts,
                                    const std::vector<std::array<std::size_t, 3>>& tris,
                                    long targetQuads, double scaling, double adaptivity) {
    SeamlessSolveResult result;
    if (verts.empty() || tris.empty()) {
        return result;
    }

    ensureGeogramInitialized();

    std::vector<AutoRemesher::Vector3> arVerts;
    arVerts.reserve(verts.size());
    for (const auto& v : verts) {
        arVerts.emplace_back(v[0], v[1], v[2]);
    }
    std::vector<std::vector<size_t>> arTris;
    arTris.reserve(tris.size());
    for (const auto& t : tris) {
        arTris.push_back({t[0], t[1], t[2]});
    }

    AutoRemesher::AutoRemesher remesher(arVerts, arTris);
    remesher.setTargetTriangleCount(static_cast<size_t>(targetQuads) * 2);  // GUI mapping
    remesher.setScaling(scaling);
    remesher.setGradientAdaptivity(adaptivity);
    remesher.setModelType(AutoRemesher::ModelType::Organic);
    if (!remesher.remesh()) {
        return result;
    }

    const std::vector<AutoRemesher::Vector3>& isoVerts = remesher.isotropicVertices();
    const std::vector<std::vector<size_t>>& isoTris = remesher.isotropicTriangles();
    const std::vector<std::vector<AutoRemesher::Vector2>>& isoUvs = remesher.isotropicTriangleUvs();
    if (isoVerts.empty() || isoTris.empty() || isoUvs.size() != isoTris.size()) {
        return result;
    }

    result.vertices.reserve(isoVerts.size());
    for (const AutoRemesher::Vector3& v : isoVerts) {
        result.vertices.push_back({v.x(), v.y(), v.z()});
    }
    result.triangles.reserve(isoTris.size());
    result.triangleUvs.reserve(isoTris.size());
    for (size_t t = 0; t < isoTris.size(); ++t) {
        if (isoTris[t].size() != 3 || isoUvs[t].size() != 3) {
            return SeamlessSolveResult{};  // malformed corner count -> fail cleanly
        }
        result.triangles.push_back({isoTris[t][0], isoTris[t][1], isoTris[t][2]});
        result.triangleUvs.push_back({isoUvs[t][0].x(), isoUvs[t][0].y(), isoUvs[t][1].x(),
                                      isoUvs[t][1].y(), isoUvs[t][2].x(), isoUvs[t][2].y()});
    }

    result.ok = true;
    return result;
}

}  // namespace cyber::remesh::qcsolver
