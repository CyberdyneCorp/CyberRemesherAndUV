// Meshlet clustering (openspec add-meshlet-target-path, task 3.1).
//
// The load-bearing properties are CONSERVATIVENESS and COMPLETENESS: a bounding
// sphere that is too small or a normal cone that is too tight makes the GPU cull
// geometry the user can see, which shows up as holes in the model. Every bound
// here is therefore asserted to CONTAIN what it claims to, not merely to exist.

#include <doctest.h>

#include <cmath>
#include <set>
#include <vector>

#include "cyber/core/meshlet.hpp"

using cyber::Vec3;
namespace remesh = cyber::remesh;

namespace {

/// A grid of `n`x`n` quads, each split into two triangles, on z = 0.
struct Grid {
    std::vector<float> positions;
    std::vector<std::uint32_t> indices;
};

Grid makeGrid(int n) {
    Grid grid;
    for (int y = 0; y <= n; ++y) {
        for (int x = 0; x <= n; ++x) {
            grid.positions.push_back(static_cast<float>(x));
            grid.positions.push_back(static_cast<float>(y));
            grid.positions.push_back(0.0f);
        }
    }
    const int stride = n + 1;
    for (int y = 0; y < n; ++y) {
        for (int x = 0; x < n; ++x) {
            const std::uint32_t a = static_cast<std::uint32_t>(y * stride + x);
            const std::uint32_t b = a + 1;
            const std::uint32_t c = a + static_cast<std::uint32_t>(stride) + 1;
            const std::uint32_t d = a + static_cast<std::uint32_t>(stride);
            grid.indices.insert(grid.indices.end(), {a, b, c});
            grid.indices.insert(grid.indices.end(), {a, c, d});
        }
    }
    return grid;
}

/// A closed octahedron — normals span every direction, so its clusters exercise
/// the cone logic on genuinely varied geometry.
Grid makeOctahedron() {
    Grid mesh;
    const float p[6][3] = {{1, 0, 0}, {-1, 0, 0}, {0, 1, 0}, {0, -1, 0}, {0, 0, 1}, {0, 0, -1}};
    for (const auto& v : p) {
        mesh.positions.insert(mesh.positions.end(), {v[0], v[1], v[2]});
    }
    const std::uint32_t faces[8][3] = {{0, 2, 4}, {2, 1, 4}, {1, 3, 4}, {3, 0, 4},
                                       {2, 0, 5}, {1, 2, 5}, {3, 1, 5}, {0, 3, 5}};
    for (const auto& f : faces) {
        mesh.indices.insert(mesh.indices.end(), {f[0], f[1], f[2]});
    }
    return mesh;
}

Vec3 positionAt(const std::vector<float>& positions, std::uint32_t v) {
    return Vec3{positions[v * 3], positions[v * 3 + 1], positions[v * 3 + 2]};
}

}  // namespace

TEST_CASE("meshlets cover every triangle exactly once") {
    const Grid grid = makeGrid(20);  // 800 triangles
    const remesh::MeshletSet set = remesh::buildMeshlets(grid.positions, grid.indices);

    REQUIRE(!set.empty());
    CHECK(set.triangleCount() == grid.indices.size() / 3);

    // Reconstruct the global triangle set from the clusters and compare with the
    // input: no triangle dropped, none duplicated into two clusters.
    std::multiset<std::array<std::uint32_t, 3>> rebuilt;
    for (const remesh::Meshlet& meshlet : set.meshlets) {
        for (std::uint32_t t = 0; t < meshlet.triangleCount; ++t) {
            const std::size_t base = (static_cast<std::size_t>(meshlet.triangleOffset) + t) * 3;
            std::array<std::uint32_t, 3> tri{};
            for (int i = 0; i < 3; ++i) {
                const std::uint8_t localIndex = set.indices[base + static_cast<std::size_t>(i)];
                CHECK(localIndex < meshlet.vertexCount);
                tri[static_cast<std::size_t>(i)] =
                    set.vertices[meshlet.vertexOffset + localIndex];
            }
            rebuilt.insert(tri);
        }
    }
    std::multiset<std::array<std::uint32_t, 3>> expected;
    for (std::size_t t = 0; t < grid.indices.size() / 3; ++t) {
        expected.insert({grid.indices[t * 3], grid.indices[t * 3 + 1], grid.indices[t * 3 + 2]});
    }
    CHECK(rebuilt == expected);
}

TEST_CASE("meshlets respect the vertex and triangle caps") {
    const Grid grid = makeGrid(24);
    const remesh::MeshletCaps caps{.maxVertices = 64, .maxTriangles = 126};
    const remesh::MeshletSet set = remesh::buildMeshlets(grid.positions, grid.indices, caps);
    REQUIRE(!set.empty());
    for (const remesh::Meshlet& meshlet : set.meshlets) {
        CHECK(meshlet.vertexCount <= caps.maxVertices);
        CHECK(meshlet.triangleCount <= caps.maxTriangles);
        CHECK(meshlet.vertexCount > 0);
        CHECK(meshlet.triangleCount > 0);
    }
    // Clusters should be reasonably full, or the clustering is not doing its job:
    // 1152 triangles at 126 per cluster is ~10 clusters, not ~1000.
    CHECK(set.meshlets.size() < grid.indices.size() / 3 / 4);
}

TEST_CASE("every bounding sphere CONTAINS its cluster's vertices") {
    // A sphere that is too small culls visible geometry. Conservativeness is the
    // property, so it is asserted directly rather than inferred from the formula.
    const Grid mesh = makeOctahedron();
    const remesh::MeshletSet set = remesh::buildMeshlets(mesh.positions, mesh.indices);
    REQUIRE(!set.empty());
    for (const remesh::Meshlet& meshlet : set.meshlets) {
        for (std::uint32_t i = 0; i < meshlet.vertexCount; ++i) {
            const Vec3 p = positionAt(mesh.positions, set.vertices[meshlet.vertexOffset + i]);
            const float distance = length(p - meshlet.center);
            CHECK(distance <= meshlet.radius + 1e-5f);
        }
    }
}

TEST_CASE("every normal cone CONTAINS its cluster's triangle normals") {
    const Grid mesh = makeOctahedron();
    const remesh::MeshletSet set = remesh::buildMeshlets(mesh.positions, mesh.indices);
    REQUIRE(!set.empty());
    for (const remesh::Meshlet& meshlet : set.meshlets) {
        if (meshlet.coneCutoff <= 0.0f) {
            continue;  // declared unusable; never culled, nothing to contain
        }
        for (std::uint32_t t = 0; t < meshlet.triangleCount; ++t) {
            const std::size_t base = (static_cast<std::size_t>(meshlet.triangleOffset) + t) * 3;
            const std::uint32_t a = set.vertices[meshlet.vertexOffset + set.indices[base]];
            const std::uint32_t b = set.vertices[meshlet.vertexOffset + set.indices[base + 1]];
            const std::uint32_t c = set.vertices[meshlet.vertexOffset + set.indices[base + 2]];
            const Vec3 pa = positionAt(mesh.positions, a);
            Vec3 n = cross(positionAt(mesh.positions, b) - pa, positionAt(mesh.positions, c) - pa);
            const float len = length(n);
            if (len <= 0.0f) {
                continue;
            }
            n = n * (1.0f / len);
            // Inside the cone means at least as aligned as the declared cutoff.
            CHECK(dot(meshlet.coneAxis, n) >= meshlet.coneCutoff - 1e-4f);
        }
    }
}

TEST_CASE("a flat grid cluster has a tight cone; a hemisphere-spanning one is unusable") {
    // Flat geometry: every normal identical, so the cone should be maximally
    // tight — this is what makes backface culling effective at all.
    const Grid grid = makeGrid(8);
    const remesh::MeshletSet flat = remesh::buildMeshlets(grid.positions, grid.indices);
    REQUIRE(!flat.empty());
    for (const remesh::Meshlet& meshlet : flat.meshlets) {
        CHECK(meshlet.coneCutoff > 0.99f);
    }

    // A cluster forced to hold opposite-facing triangles must declare itself
    // unusable (cutoff <= 0) rather than claim a cone that would cull visible
    // faces. Two back-to-back triangles, one cluster.
    std::vector<float> positions{0, 0, 0, 1, 0, 0, 0, 1, 0};
    std::vector<std::uint32_t> indices{0, 1, 2, 0, 2, 1};  // opposite windings
    const remesh::MeshletSet folded = remesh::buildMeshlets(positions, indices);
    REQUIRE(folded.meshlets.size() == 1);
    CHECK(folded.meshlets[0].triangleCount == 2);
    CHECK(folded.meshlets[0].coneCutoff == 0.0f);
}

TEST_CASE("clustering is deterministic") {
    const Grid grid = makeGrid(16);
    const remesh::MeshletSet a = remesh::buildMeshlets(grid.positions, grid.indices);
    const remesh::MeshletSet b = remesh::buildMeshlets(grid.positions, grid.indices);
    REQUIRE(a.meshlets.size() == b.meshlets.size());
    CHECK(a.vertices == b.vertices);
    CHECK(a.indices == b.indices);
    for (std::size_t i = 0; i < a.meshlets.size(); ++i) {
        CHECK(a.meshlets[i].vertexOffset == b.meshlets[i].vertexOffset);
        CHECK(a.meshlets[i].triangleCount == b.meshlets[i].triangleCount);
        CHECK(a.meshlets[i].coneCutoff == b.meshlets[i].coneCutoff);
    }
}

TEST_CASE("malformed input yields an EMPTY set, never a partial one") {
    const Grid grid = makeGrid(4);
    // Out-of-range index: a partially valid set would be drawn by the renderer.
    std::vector<std::uint32_t> bad = grid.indices;
    bad[0] = 9999;
    CHECK(remesh::buildMeshlets(grid.positions, bad).empty());
    // Index count not a multiple of 3.
    std::vector<std::uint32_t> ragged{0, 1};
    CHECK(remesh::buildMeshlets(grid.positions, ragged).empty());
    CHECK(remesh::buildMeshlets(grid.positions, {}).empty());
}

TEST_CASE("degenerate triangles are dropped, not clustered") {
    // A zero-area triangle has no normal, so keeping it would widen a cone for
    // nothing. The rest of the mesh must still cluster.
    std::vector<float> positions{0, 0, 0, 1, 0, 0, 0, 1, 0, 1, 1, 0};
    std::vector<std::uint32_t> indices{
        0, 1, 2,  // real
        0, 1, 1,  // degenerate (repeated corner)
        1, 3, 2,  // real
    };
    const remesh::MeshletSet set = remesh::buildMeshlets(positions, indices);
    REQUIRE(!set.empty());
    CHECK(set.triangleCount() == 2);
}
