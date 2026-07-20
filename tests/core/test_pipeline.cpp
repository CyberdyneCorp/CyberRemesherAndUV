#include <doctest.h>

#include <cmath>
#include <vector>

#include "cyber/core/pipeline.hpp"

using cyber::CancelToken;
using cyber::FaceId;
using cyber::Index;
using cyber::Mesh;
using cyber::ProgressSink;
using cyber::Vec3;
using cyber::VertexId;
namespace remesh = cyber::remesh;

namespace {

Mesh makeSphere(int rings, int segments, Vec3 center = {}, float radius = 1.0f) {
    std::vector<Vec3> p;
    p.push_back(center + Vec3{0, 0, radius});
    for (int r = 1; r < rings; ++r) {
        const float phi = cyber::kPi * static_cast<float>(r) / static_cast<float>(rings);
        for (int s = 0; s < segments; ++s) {
            const float theta =
                2.0f * cyber::kPi * static_cast<float>(s) / static_cast<float>(segments);
            p.push_back(center + Vec3{std::sin(phi) * std::cos(theta) * radius,
                                      std::sin(phi) * std::sin(theta) * radius,
                                      std::cos(phi) * radius});
        }
    }
    p.push_back(center + Vec3{0, 0, -radius});
    const Index south = static_cast<Index>(p.size() - 1);

    std::vector<std::vector<Index>> f;
    auto ring = [segments](int r, int s) {
        return static_cast<Index>(1 + (r - 1) * segments + (s % segments));
    };
    for (int s = 0; s < segments; ++s) {
        f.push_back({0, ring(1, s), ring(1, s + 1)});
    }
    for (int r = 1; r + 1 < rings; ++r) {
        for (int s = 0; s < segments; ++s) {
            f.push_back({ring(r, s), ring(r + 1, s), ring(r + 1, s + 1)});
            f.push_back({ring(r, s), ring(r + 1, s + 1), ring(r, s + 1)});
        }
    }
    for (int s = 0; s < segments; ++s) {
        f.push_back({south, ring(rings - 1, s + 1), ring(rings - 1, s)});
    }
    return Mesh::fromIndexed(p, f);
}

remesh::Parameters smallRun(int quads) {
    remesh::Parameters params;
    params.targetQuadCount = quads;
    params.adaptivity = 0.0f;
    return params;
}

}  // namespace

TEST_CASE("pipeline remeshes a sphere into a quad-dominant mesh") {
    const Mesh sphere = makeSphere(12, 18);
    const auto result = remesh::remesh(sphere, smallRun(400));
    REQUIRE(result.status == remesh::RunStatus::Success);
    REQUIRE(result.mesh.validate().empty());
    REQUIRE(result.stats.islandCount == 1);
    REQUIRE(result.stats.islandsFailed == 0);
    // Quad-dominant: strictly more quads than leftover triangles.
    REQUIRE(result.stats.quadCount > result.stats.triangleCount);
    // Face count in the right ballpark of the request (0.5x - 2x).
    const std::size_t total = result.stats.quadCount + result.stats.triangleCount;
    REQUIRE(total > 200);
    REQUIRE(total < 800);
}

TEST_CASE("adaptive refinement does not compound across iterations (runaway regression)") {
    // Regression: scales were once recomputed from the current mesh every
    // iteration; with curvature variance (pole fans) and a large target this
    // compounded up to 0.3^iterations of the target edge length — a 100x
    // face explosion observed via the CLI. Scales are now computed once from
    // the input geometry.
    const Mesh sphere = makeSphere(24, 36);  // pole fans concentrate curvature
    remesh::Parameters params;
    params.targetQuadCount = 2000;  // adaptivity stays at its default 1.0
    const auto result = remesh::remesh(sphere, params);
    REQUIRE(result.status == remesh::RunStatus::Success);
    const std::size_t total = result.stats.quadCount + result.stats.triangleCount;
    REQUIRE(total > 1000);
    REQUIRE(total < 5000);
}

TEST_CASE("default adaptivity stays near-uniform on a uniform-curvature surface") {
    // Regression: mean-normalized adaptive scaling must not coarsen a sphere
    // (max-normalization once produced ~4x fewer faces than requested).
    const Mesh sphere = makeSphere(16, 24);
    remesh::Parameters params;
    params.targetQuadCount = 400;  // adaptivity stays at its default 1.0
    const auto result = remesh::remesh(sphere, params);
    REQUIRE(result.status == remesh::RunStatus::Success);
    const std::size_t total = result.stats.quadCount + result.stats.triangleCount;
    REQUIRE(total > 200);
    REQUIRE(total < 800);
}

TEST_CASE("empty input is an error, not a crash (spec: empty input)") {
    const Mesh empty;
    const auto result = remesh::remesh(empty, smallRun(400));
    REQUIRE(result.status == remesh::RunStatus::Error);
    REQUIRE(result.error.find("empty") != std::string::npos);
}

TEST_CASE("multi-island input remeshes every island (spec: multi-island)") {
    // Two disconnected spheres.
    Mesh sphereA = makeSphere(10, 14, {0, 0, 0});
    const Mesh sphereB = makeSphere(10, 14, {5, 0, 0});
    std::vector<Vec3> pa, pb;
    std::vector<std::vector<Index>> fa, fb;
    sphereA.toIndexed(pa, fa);
    sphereB.toIndexed(pb, fb);
    const Index offset = static_cast<Index>(pa.size());
    for (auto& face : fb) {
        for (Index& v : face) {
            v += offset;
        }
        fa.push_back(face);
    }
    pa.insert(pa.end(), pb.begin(), pb.end());
    const Mesh both = Mesh::fromIndexed(pa, fa);

    const auto result = remesh::remesh(both, smallRun(500));
    REQUIRE(result.status == remesh::RunStatus::Success);
    REQUIRE(result.stats.islandCount == 2);
    REQUIRE(result.mesh.islands().size() == 2);  // both shells in the output
}

// Regression: an unwelded "polygon soup" (patches meeting at coincident-but-distinct
// vertices) must be fused into one connected surface before islands(), otherwise each
// patch is quadrangulated independently and their shared boundaries leave open seams at
// high density (the flat-cube -> >=1500-quad stitching bug: hundreds of boundary edges).
// A unit square as two triangles sharing a diagonal in POSITION only (no shared vertex
// index) is two islands until welded. Genuinely-separate geometry (the two spheres
// above, 5 units apart) is untouched — the weld merges only coincident vertices.
TEST_CASE("pipeline welds coincident vertices so unwelded patches fuse into one surface") {
    const std::vector<Vec3> p = {{0, 0, 0}, {1, 0, 0}, {1, 1, 0},   // triangle A
                                 {0, 0, 0}, {1, 1, 0}, {0, 1, 0}};  // triangle B: dups A's diag
    const std::vector<std::vector<Index>> f = {{0, 1, 2}, {3, 4, 5}};
    const Mesh soup = Mesh::fromIndexed(p, f);
    REQUIRE(soup.islands().size() == 2);  // unwelded: two disconnected patches

    const auto result = remesh::remesh(soup, smallRun(64));
    REQUIRE(result.status == remesh::RunStatus::Success);
    CHECK(result.mesh.islands().size() == 1);  // welded -> one connected surface, no seam
}

TEST_CASE("pure quads option yields zero non-quads (spec: pure-quad post-pass)") {
    const Mesh sphere = makeSphere(12, 18);
    remesh::Parameters params = smallRun(400);
    params.pureQuads = true;
    const auto result = remesh::remesh(sphere, params);
    REQUIRE(result.status == remesh::RunStatus::Success);
    REQUIRE(result.stats.triangleCount == 0);
    REQUIRE(result.stats.otherPolygonCount == 0);
    REQUIRE(result.stats.quadCount > 0);
    REQUIRE(result.mesh.validate().empty());
}

TEST_CASE("parameter clamp warnings surface in the result (spec)") {
    const Mesh sphere = makeSphere(8, 12);
    remesh::Parameters params = smallRun(300);
    params.edgeScale = 10.0f;
    const auto result = remesh::remesh(sphere, params);
    REQUIRE(result.status == remesh::RunStatus::Success);
    REQUIRE(result.parameterIssues.size() == 1);
    REQUIRE(result.parameterIssues[0].parameter == "edgeScale");
}

TEST_CASE("cancellation returns Cancelled and an empty result (spec: atomic)") {
    const Mesh sphere = makeSphere(12, 18);
    const CancelToken cancel;
    cancel.requestCancel();
    const auto result = remesh::remesh(sphere, smallRun(400), nullptr, &cancel);
    REQUIRE(result.status == remesh::RunStatus::Cancelled);
    REQUIRE(result.mesh.faceCount() == 0);  // nothing committed
}

TEST_CASE("progress is monotonic and finishes at 1.0") {
    const Mesh sphere = makeSphere(10, 14);
    std::vector<float> values;
    ProgressSink sink([&values](float p, std::string_view) { values.push_back(p); });
    const auto result = remesh::remesh(sphere, smallRun(300), &sink);
    REQUIRE(result.status == remesh::RunStatus::Success);
    REQUIRE(!values.empty());
    for (std::size_t i = 1; i < values.size(); ++i) {
        REQUIRE(values[i] >= values[i - 1]);
    }
    REQUIRE(values.back() == doctest::Approx(1.0f));
}

TEST_CASE("pipeline output is deterministic") {
    const Mesh sphere = makeSphere(10, 14);
    auto run = [&sphere] {
        const auto result = remesh::remesh(sphere, smallRun(300));
        std::vector<Vec3> positions;
        std::vector<std::vector<Index>> faces;
        result.mesh.toIndexed(positions, faces);
        return std::pair{positions, faces};
    };
    const auto [p1, f1] = run();
    const auto [p2, f2] = run();
    REQUIRE(f1 == f2);
    REQUIRE(p1.size() == p2.size());
    for (std::size_t i = 0; i < p1.size(); ++i) {
        REQUIRE(p1[i] == p2[i]);
    }
}

TEST_CASE("small patch policies (spec: disconnected patch policy)") {
    // A big grid patch and a tiny separate triangle.
    std::vector<Vec3> p;
    std::vector<std::vector<Index>> f;
    for (int y = 0; y <= 3; ++y) {
        for (int x = 0; x <= 3; ++x) {
            p.push_back({static_cast<float>(x), static_cast<float>(y), 0});
        }
    }
    for (Index y = 0; y < 3; ++y) {
        for (Index x = 0; x < 3; ++x) {
            const Index i = y * 4 + x;
            f.push_back({i, i + 1, i + 5, i + 4});
        }
    }
    const Index base = static_cast<Index>(p.size());
    p.push_back({10, 0, 0});
    p.push_back({11, 0, 0});
    p.push_back({10, 1, 0});
    f.push_back({base, base + 1, base + 2});

    Mesh keepLargest = Mesh::fromIndexed(p, f);
    remesh::applySmallPatchPolicy(keepLargest, remesh::SmallPatchPolicy::KeepLargest, 0);
    REQUIRE(keepLargest.islands().size() == 1);
    REQUIRE(keepLargest.faceCount() == 9);

    Mesh keepAll = Mesh::fromIndexed(p, f);
    remesh::applySmallPatchPolicy(keepAll, remesh::SmallPatchPolicy::KeepAll, 0);
    REQUIRE(keepAll.islands().size() == 2);  // AutoRemesher always discarded these

    Mesh minFaces = Mesh::fromIndexed(p, f);
    remesh::applySmallPatchPolicy(minFaces, remesh::SmallPatchPolicy::MinFaces, 5);
    REQUIRE(minFaces.islands().size() == 1);
}
