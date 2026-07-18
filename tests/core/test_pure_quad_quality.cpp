#include <doctest.h>

#include <array>
#include <cmath>
#include <map>
#include <utility>
#include <vector>

#include "cyber/core/math.hpp"
#include "cyber/core/mesh.hpp"
#include "cyber/core/pipeline.hpp"
#include "cyber/core/remesh_params.hpp"

using cyber::Index;
using cyber::Mesh;
using cyber::Vec3;
namespace remesh = cyber::remesh;

namespace {

// A clean icosphere (subdivided icosahedron projected to the unit sphere):
// uniform, watertight, no poles/seams/duplicate vertices, so the quads a
// remesher produces reflect the algorithm rather than a degenerate input.
Mesh makeIcosphere(int subdivisions) {
    const float t = (1.0f + std::sqrt(5.0f)) / 2.0f;
    std::vector<Vec3> pos = {{-1, t, 0}, {1, t, 0},  {-1, -t, 0}, {1, -t, 0},
                             {0, -1, t}, {0, 1, t},  {0, -1, -t}, {0, 1, -t},
                             {t, 0, -1}, {t, 0, 1},  {-t, 0, -1}, {-t, 0, 1}};
    std::vector<std::array<int, 3>> tris = {
        {0, 11, 5}, {0, 5, 1},  {0, 1, 7},  {0, 7, 10}, {0, 10, 11}, {1, 5, 9},   {5, 11, 4},
        {11, 10, 2}, {10, 7, 6}, {7, 1, 8}, {3, 9, 4},  {3, 4, 2},   {3, 2, 6},   {3, 6, 8},
        {3, 8, 9},   {4, 9, 5},  {2, 4, 11}, {6, 2, 10}, {8, 6, 7},   {9, 8, 1}};
    for (int s = 0; s < subdivisions; ++s) {
        std::vector<std::array<int, 3>> next;
        std::map<std::pair<int, int>, int> midCache;
        auto midpoint = [&](int a, int b) {
            const auto key = std::minmax(a, b);
            const auto it = midCache.find({key.first, key.second});
            if (it != midCache.end()) {
                return it->second;
            }
            const Vec3 m = (pos[static_cast<std::size_t>(a)] + pos[static_cast<std::size_t>(b)]) *
                           0.5f;
            const int idx = static_cast<int>(pos.size());
            pos.push_back(m);
            midCache[{key.first, key.second}] = idx;
            return idx;
        };
        for (const auto& tr : tris) {
            const int a = midpoint(tr[0], tr[1]);
            const int b = midpoint(tr[1], tr[2]);
            const int c = midpoint(tr[2], tr[0]);
            next.push_back({tr[0], a, c});
            next.push_back({tr[1], b, a});
            next.push_back({tr[2], c, b});
            next.push_back({a, b, c});
        }
        tris = std::move(next);
    }
    std::vector<Vec3> normalizedPos;
    std::vector<std::vector<Index>> faces;
    normalizedPos.reserve(pos.size());
    for (const Vec3& p : pos) {
        normalizedPos.push_back(cyber::normalized(p));
    }
    for (const auto& tr : tris) {
        faces.push_back({static_cast<Index>(tr[0]), static_cast<Index>(tr[1]),
                         static_cast<Index>(tr[2])});
    }
    return Mesh::fromIndexed(normalizedPos, faces);
}

struct QuadQuality {
    std::size_t quads = 0;
    std::size_t nonQuads = 0;
    float worstMinAngleDeg = 180.0f;  // smallest interior angle across all quads
    float shortestEdgeRatio = 1.0f;   // min edge / mean edge (0 => a collapsed quad)
    float maxRadiusError = 0.0f;      // |‖v‖ - 1| over all vertices (unit sphere)
};

QuadQuality measure(const Mesh& mesh) {
    std::vector<Vec3> P;
    std::vector<std::vector<Index>> F;
    mesh.toIndexed(P, F);

    // Mean quad edge length, to judge collapse in a scale-independent way.
    double edgeSum = 0.0;
    std::size_t edgeN = 0;
    for (const auto& f : F) {
        if (f.size() != 4) {
            continue;
        }
        for (std::size_t k = 0; k < 4; ++k) {
            edgeSum += static_cast<double>(cyber::length(P[f[k]] - P[f[(k + 1) % 4]]));
            ++edgeN;
        }
    }
    const float meanEdge = edgeN ? static_cast<float>(edgeSum / static_cast<double>(edgeN)) : 1.0f;

    QuadQuality q;
    float shortestEdge = meanEdge;
    for (const Vec3& p : P) {
        q.maxRadiusError = std::fmax(q.maxRadiusError, std::fabs(cyber::length(p) - 1.0f));
    }
    for (const auto& f : F) {
        if (f.size() != 4) {
            ++q.nonQuads;
            continue;
        }
        ++q.quads;
        for (std::size_t k = 0; k < 4; ++k) {
            const Vec3 a = P[f[k]];
            const Vec3 e1 = P[f[(k + 1) % 4]] - a;
            const Vec3 e2 = P[f[(k + 3) % 4]] - a;
            shortestEdge = std::fmin(shortestEdge, cyber::length(e1));
            const float l1 = cyber::length(e1);
            const float l2 = cyber::length(e2);
            const float c = dot(e1, e2) / (l1 * l2 + 1e-12f);
            const float ang =
                std::acos(std::clamp(c, -1.0f, 1.0f)) * 180.0f / 3.14159265358979323846f;
            q.worstMinAngleDeg = std::fmin(q.worstMinAngleDeg, ang);
        }
    }
    q.shortestEdgeRatio = meanEdge > 0.0f ? shortestEdge / meanEdge : 0.0f;
    return q;
}

}  // namespace

// Pure-quad conversion of a clean closed surface must yield 100% quads with
// GOOD element quality — no slivers, no collapsed edges — while preserving the
// shape. This guards the tangential-relaxation de-slivering pass in the
// pure-quad path (pipeline.cpp relaxQuadMesh): without it the linear-subdivide
// + project construction leaves >50% degenerate quads (worst angle ~0 deg).
TEST_CASE("pure-quad remeshing produces well-shaped quads on a clean sphere") {
    const Mesh sphere = makeIcosphere(3);  // 1280 triangles, unit radius

    for (const int target : {800, 2500}) {
        CAPTURE(target);
        remesh::Parameters params;
        params.targetQuadCount = target;
        params.pureQuads = true;
        const remesh::PipelineResult res = remesh::remesh(sphere, params);
        REQUIRE(res.status == remesh::RunStatus::Success);

        const QuadQuality q = measure(res.mesh);
        CAPTURE(q.worstMinAngleDeg);
        CAPTURE(q.shortestEdgeRatio);
        CAPTURE(q.maxRadiusError);

        CHECK(q.quads > 0);
        CHECK(q.nonQuads == 0);                 // genuinely pure quads
        CHECK(q.worstMinAngleDeg > 30.0f);      // no slivers (relaxation ~51 deg)
        CHECK(q.shortestEdgeRatio > 0.15f);     // no collapsed edges
        CHECK(q.maxRadiusError < 0.01f);        // shape preserved on the unit sphere
        CHECK(res.mesh.validate().empty());     // still a valid manifold
    }
}

// Determinism: identical input and parameters yield a byte-identical indexed
// mesh (the relaxation is a fixed iteration count, no randomness).
TEST_CASE("pure-quad remeshing is deterministic") {
    const Mesh sphere = makeIcosphere(3);
    remesh::Parameters params;
    params.targetQuadCount = 1200;
    params.pureQuads = true;

    const remesh::PipelineResult a = remesh::remesh(sphere, params);
    const remesh::PipelineResult b = remesh::remesh(sphere, params);
    REQUIRE(a.status == remesh::RunStatus::Success);
    REQUIRE(b.status == remesh::RunStatus::Success);

    std::vector<Vec3> pa, pb;
    std::vector<std::vector<Index>> fa, fb;
    a.mesh.toIndexed(pa, fa);
    b.mesh.toIndexed(pb, fb);
    CHECK(pa == pb);
    CHECK(fa == fb);
}
