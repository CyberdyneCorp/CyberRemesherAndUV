#include <doctest.h>

#include <vector>

#include "cyber/core/mesh.hpp"
#include "cyber/render/camera.hpp"
#include "cyber/render/mesh_stream.hpp"

using namespace cyber;
using namespace cyber::render;

namespace {

// A flat cells x cells quad grid in the y = 0 plane spanning [0, size]^2.
Mesh makeGrid(int cells, float size) {
    const int pts = cells + 1;
    std::vector<Vec3> positions;
    positions.reserve(static_cast<std::size_t>(pts) * static_cast<std::size_t>(pts));
    for (int row = 0; row < pts; ++row) {
        for (int col = 0; col < pts; ++col) {
            const float x = static_cast<float>(col) / static_cast<float>(cells) * size;
            const float z = static_cast<float>(row) / static_cast<float>(cells) * size;
            positions.push_back(Vec3{x, 0.0f, z});
        }
    }
    std::vector<std::vector<Index>> faces;
    for (int row = 0; row < cells; ++row) {
        for (int col = 0; col < cells; ++col) {
            const auto a = static_cast<Index>(row * pts + col);
            const auto b = static_cast<Index>(row * pts + col + 1);
            const auto c = static_cast<Index>((row + 1) * pts + col + 1);
            const auto d = static_cast<Index>((row + 1) * pts + col);
            faces.push_back({a, b, c, d});
        }
    }
    return Mesh::fromIndexed(positions, faces);
}

// A frustum whose planes all point inward with a huge margin: contains
// everything, isolating the LOD/budget logic from projection math.
Frustum everythingFrustum() {
    Frustum f;
    const float big = 1e6f;
    f.planes[0] = Plane{Vec3{1, 0, 0}, big};
    f.planes[1] = Plane{Vec3{-1, 0, 0}, big};
    f.planes[2] = Plane{Vec3{0, 1, 0}, big};
    f.planes[3] = Plane{Vec3{0, -1, 0}, big};
    f.planes[4] = Plane{Vec3{0, 0, 1}, big};
    f.planes[5] = Plane{Vec3{0, 0, -1}, big};
    return f;
}

}  // namespace

TEST_CASE("Aabb expand tracks the tight bounds") {
    Aabb box;
    CHECK(box.empty());
    box.expand(Vec3{1, 2, 3});
    box.expand(Vec3{-1, 5, 0});
    CHECK_FALSE(box.empty());
    CHECK(box.min.x == doctest::Approx(-1.0f));
    CHECK(box.max.y == doctest::Approx(5.0f));
    CHECK(box.center().z == doctest::Approx(1.5f));
}

TEST_CASE("build partitions the mesh and counts every triangle") {
    const int cells = 16;
    const Mesh mesh = makeGrid(cells, 10.0f);
    MeshStreamManager mgr;
    StreamConfig config;
    config.gridResolution = 4;
    mgr.build(mesh, config);

    const std::uint64_t quads = static_cast<std::uint64_t>(cells) * static_cast<std::uint64_t>(cells);
    CHECK(mgr.totalTriangles() == quads * 2);  // each quad fan-triangulates to 2
    CHECK_FALSE(mgr.chunks().empty());

    // Every chunk's bounds must lie inside the overall mesh bounds.
    const Aabb& all = mgr.bounds();
    for (const MeshChunk& chunk : mgr.chunks()) {
        CHECK(chunk.bounds.min.x >= all.min.x - 1e-3f);
        CHECK(chunk.bounds.max.x <= all.max.x + 1e-3f);
        CHECK(chunk.triangleCount > 0);
    }
}

TEST_CASE("selectVisible returns all chunks with a containing frustum") {
    const Mesh mesh = makeGrid(8, 4.0f);
    MeshStreamManager mgr;
    mgr.build(mesh);

    SelectionStats stats;
    const std::vector<VisibleChunk> visible =
        mgr.selectVisible(everythingFrustum(), Vec3{0, 10, 0}, &stats);

    CHECK(stats.culledByFrustum == 0);
    CHECK(visible.size() == mgr.chunks().size());
    // Front-to-back ordering.
    for (std::size_t i = 1; i < visible.size(); ++i) {
        CHECK(visible[i - 1].distance <= visible[i].distance + 1e-4f);
    }
}

TEST_CASE("triangle budget drops the farthest chunks") {
    const Mesh mesh = makeGrid(16, 10.0f);
    MeshStreamManager mgr;
    StreamConfig config;
    config.gridResolution = 8;
    config.triangleBudget = 100;  // far below the full triangle count
    mgr.build(mesh, config);

    SelectionStats stats;
    const std::vector<VisibleChunk> visible =
        mgr.selectVisible(everythingFrustum(), Vec3{5, 20, 5}, &stats);

    CHECK(stats.droppedByBudget > 0);
    CHECK(stats.selectedTriangles <= config.triangleBudget + mgr.chunks().front().triangleCount);
    CHECK(visible.size() < mgr.chunks().size());
}

TEST_CASE("higher LOD selects fewer indices than LOD 0") {
    const Mesh mesh = makeGrid(8, 4.0f);
    MeshStreamManager mgr;
    StreamConfig config;
    config.lodCount = 3;
    mgr.build(mesh, config);

    const std::uint32_t chunk0 = 0;
    const ChunkLod lod0 = mgr.lodRange(chunk0, 0);
    const ChunkLod lod2 = mgr.lodRange(chunk0, 2);
    CHECK(lod0.indexCount >= lod2.indexCount);
    CHECK(lod0.indexOffset == lod2.indexOffset);
}

TEST_CASE("frustum from a real camera keeps geometry in front and rejects behind") {
    OrbitCamera cam;
    cam.setTarget(Vec3{0, 0, 0});
    cam.setDistance(5.0f);
    cam.orbit(0.0f, 0.0f);
    const Mat4 view = cam.viewMatrix();
    const Mat4 proj = cam.projectionMatrix(1.0f);
    const Frustum frustum = Frustum::fromViewProjection(multiply(proj, view));

    Aabb inFront;
    inFront.expand(cam.target() + Vec3{-0.5f, -0.5f, -0.5f});
    inFront.expand(cam.target() + Vec3{0.5f, 0.5f, 0.5f});
    CHECK(frustum.intersects(inFront));

    // A box far behind the camera eye.
    const Vec3 behind = cam.eye() + (cam.eye() - cam.target());
    Aabb behindBox;
    behindBox.expand(behind + Vec3{-0.5f, -0.5f, -0.5f});
    behindBox.expand(behind + Vec3{0.5f, 0.5f, 0.5f});
    CHECK_FALSE(frustum.intersects(behindBox));
}
