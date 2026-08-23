// Conform to an updated Target (pipeline-bridge spec, "Conform to an updated
// Target").
#include <doctest.h>

#include <algorithm>
#include <cmath>
#include <functional>
#include <vector>

#include "cyber/retopo/commands.hpp"
#include "cyber/retopo/conform.hpp"
#include "cyber/retopo/pins.hpp"
#include "cyber/retopo/snapping.hpp"

using cyber::FaceId;
using cyber::Index;
using cyber::Mesh;
using cyber::Vec3;
using cyber::VertexId;
namespace retopo = cyber::retopo;

namespace {

// A triangulated grid over [0,1]^2, displaced in z by `height`.
Mesh makeGrid(int n, const std::function<float(float, float)>& height) {
    std::vector<Vec3> p;
    for (int j = 0; j <= n; ++j) {
        for (int i = 0; i <= n; ++i) {
            const float u = static_cast<float>(i) / static_cast<float>(n);
            const float v = static_cast<float>(j) / static_cast<float>(n);
            p.push_back({u, v, height(u, v)});
        }
    }
    std::vector<std::vector<Index>> f;
    const Index stride = static_cast<Index>(n) + 1;
    for (int j = 0; j < n; ++j) {
        for (int i = 0; i < n; ++i) {
            const Index a = static_cast<Index>(j * (n + 1) + i);
            f.push_back({a, a + 1, a + stride + 1});
            f.push_back({a, a + stride + 1, a + stride});
        }
    }
    return Mesh::fromIndexed(p, f);
}

// The full connectivity of a mesh: counts plus every live face's vertex list,
// in face-id order. Compared verbatim before/after to prove topology survived.
struct TopologySnapshot {
    std::size_t vertices = 0;
    std::size_t edges = 0;
    std::size_t faces = 0;
    std::vector<std::vector<Index>> faceVertices;

    bool operator==(const TopologySnapshot& other) const = default;
};

TopologySnapshot snapshot(const Mesh& mesh) {
    TopologySnapshot out;
    out.vertices = mesh.vertexCount();
    out.edges = mesh.edgeCount();
    out.faces = mesh.faceCount();
    for (Index fi = 0; fi < mesh.faceCapacity(); ++fi) {
        const FaceId f{fi};
        if (!mesh.isAlive(f)) {
            continue;
        }
        std::vector<Index> ids;
        for (const VertexId v : mesh.faceVertices(f)) {
            ids.push_back(v.value);
        }
        out.faceVertices.push_back(std::move(ids));
    }
    return out;
}

float maxDistanceToSurface(const Mesh& mesh, const retopo::SurfaceSnapper& snapper) {
    float worst = 0.0f;
    for (Index i = 0; i < mesh.vertexCapacity(); ++i) {
        if (!mesh.isAlive(VertexId{i})) {
            continue;
        }
        const retopo::SurfaceHit hit = snapper.snapToSurface(mesh.position(VertexId{i}));
        worst = std::fmax(worst, std::sqrt(hit.distanceSquared));
    }
    return worst;
}

}  // namespace

TEST_CASE("conform re-snaps the EditMesh onto a replaced Target and reports max and RMS") {
    // The EditMesh was retopologised against a flat Target...
    Mesh edit = makeGrid(6, [](float, float) { return 0.0f; });
    // ...and the sculpt then grew a dome under it.
    const Mesh newTarget = makeGrid(40, [](float u, float v) {
        return 0.12f * std::sin(u * 3.14159f) * std::sin(v * 3.14159f);
    });
    const retopo::SurfaceSnapper snapper(newTarget);

    const TopologySnapshot before = snapshot(edit);
    const retopo::ConformReport report = retopo::conform(edit, snapper);

    CHECK(snapshot(edit) == before);  // topology preserved exactly
    CHECK(report.movedVertices > 0);
    CHECK(report.maxDeviation > 0.0f);
    CHECK(report.rmsDeviation > 0.0f);
    CHECK(report.maxDeviation >= report.rmsDeviation);
    CHECK(report.flagged.empty());  // no threshold set

    // Every vertex now lies on the new surface, re-queried through the snapper.
    CHECK(maxDistanceToSurface(edit, snapper) < 1e-4f);
}

TEST_CASE("conform flags vertices past the threshold and still completes") {
    Mesh edit = makeGrid(8, [](float, float) { return 0.0f; });
    // The sculpt diverged sharply, but only over the u > 0.7 strip.
    const Mesh newTarget = makeGrid(40, [](float u, float) { return u > 0.7f ? 0.4f : 0.0f; });
    const retopo::SurfaceSnapper snapper(newTarget);

    // Which edit vertices sit in the diverged region, recorded before the move.
    std::vector<Index> inRegion;
    std::vector<Index> outsideRegion;
    for (Index i = 0; i < edit.vertexCapacity(); ++i) {
        if (!edit.isAlive(VertexId{i})) {
            continue;
        }
        (edit.position(VertexId{i}).x > 0.8f ? inRegion : outsideRegion).push_back(i);
    }
    REQUIRE(!inRegion.empty());
    REQUIRE(!outsideRegion.empty());

    const TopologySnapshot before = snapshot(edit);
    retopo::ConformParams params;
    params.deviationThreshold = 0.1f;
    const retopo::ConformReport report = retopo::conform(edit, snapper, params);

    CHECK(snapshot(edit) == before);
    REQUIRE(!report.flagged.empty());
    const auto isFlagged = [&report](Index i) {
        return std::find_if(report.flagged.begin(), report.flagged.end(),
                            [i](VertexId v) { return v.value == i; }) != report.flagged.end();
    };
    for (const Index i : inRegion) {
        CAPTURE(i);
        CHECK(isFlagged(i));
    }
    for (const Index i : outsideRegion) {
        CAPTURE(i);
        CHECK_FALSE(isFlagged(i));
    }
    // Completed, not discarded: the flagged vertices moved onto the surface too.
    CHECK(maxDistanceToSurface(edit, snapper) < 1e-4f);
    CHECK(report.maxDeviation > params.deviationThreshold);
}

TEST_CASE("conform leaves pinned vertices alone and out of the statistics") {
    Mesh edit = makeGrid(4, [](float, float) { return 0.0f; });
    const Mesh newTarget = makeGrid(20, [](float, float) { return 0.25f; });
    const retopo::SurfaceSnapper snapper(newTarget);

    retopo::PinSet pins;
    pins.pin(VertexId{0});
    const Vec3 pinnedBefore = edit.position(VertexId{0});

    retopo::ConformParams params;
    params.deviationThreshold = 0.1f;
    const retopo::ConformReport report = retopo::conform(edit, snapper, params, &pins);

    CHECK(edit.position(VertexId{0}).z == doctest::Approx(pinnedBefore.z));
    CHECK(report.movedVertices == edit.vertexCount() - 1);
    // Reported deviation is the (uniform) 0.25 lift of the UNPINNED vertices;
    // the pin contributes nothing, so max and RMS agree.
    CHECK(report.maxDeviation == doctest::Approx(0.25f).epsilon(0.01));
    CHECK(report.rmsDeviation == doctest::Approx(0.25f).epsilon(0.01));
    for (const VertexId v : report.flagged) {
        CHECK(v.value != 0u);
    }
}

TEST_CASE("conform on an unchanged Target reports zero deviation and moves nothing") {
    const Mesh target = makeGrid(20, [](float u, float v) { return 0.1f * u * v; });
    Mesh edit = makeGrid(20, [](float u, float v) { return 0.1f * u * v; });
    const retopo::SurfaceSnapper snapper(target);

    const retopo::ConformReport report = retopo::conform(edit, snapper);
    CHECK(report.maxDeviation < 1e-5f);
    CHECK(report.rmsDeviation < 1e-5f);
}

TEST_CASE("snapAll is conform with no threshold") {
    Mesh a = makeGrid(6, [](float, float) { return 0.0f; });
    Mesh b = makeGrid(6, [](float, float) { return 0.0f; });
    const Mesh target = makeGrid(30, [](float u, float) { return 0.2f * u; });
    const retopo::SurfaceSnapper snapper(target);

    const retopo::ConformReport viaConform = retopo::conform(a, snapper);
    const retopo::ConformReport viaSnapAll = retopo::snapAll(b, snapper);

    CHECK(viaSnapAll.movedVertices == viaConform.movedVertices);
    CHECK(viaSnapAll.maxDeviation == doctest::Approx(viaConform.maxDeviation));
    CHECK(viaSnapAll.rmsDeviation == doctest::Approx(viaConform.rmsDeviation));
    CHECK(viaSnapAll.flagged.empty());
    for (Index i = 0; i < a.vertexCapacity(); ++i) {
        CHECK(a.position(VertexId{i}).z == doctest::Approx(b.position(VertexId{i}).z));
    }
}
