#include <doctest.h>

#include <vector>

#include "cyber/bakecage/cage.hpp"
#include "cyber/bakecage/links.hpp"
#include "cyber/bakecage/preview.hpp"
#include "cyber/core/mesh.hpp"

using cyber::FaceId;
using cyber::Index;
using cyber::Mesh;
using cyber::Vec3;
using cyber::VertexId;
namespace bc = cyber::bakecage;

namespace {

// A (cols x rows) grid of vertices in the z = 0 plane, quad-faced, so every
// interior vertex has edge neighbours for relax/tweak tests.
Mesh makeGrid(int cols, int rows) {
    std::vector<Vec3> pos;
    for (int j = 0; j < rows; ++j) {
        for (int i = 0; i < cols; ++i) {
            pos.push_back(Vec3{static_cast<float>(i), static_cast<float>(j), 0.0f});
        }
    }
    std::vector<std::vector<Index>> faces;
    for (int j = 0; j < rows - 1; ++j) {
        for (int i = 0; i < cols - 1; ++i) {
            const Index a = static_cast<Index>(j * cols + i);
            const Index b = static_cast<Index>(j * cols + i + 1);
            const Index c = static_cast<Index>((j + 1) * cols + i + 1);
            const Index d = static_cast<Index>((j + 1) * cols + i);
            faces.push_back({a, b, c, d});
        }
    }
    return Mesh::fromIndexed(pos, faces);
}

// A single axis-aligned quad centred at (cx, cy) in the z = 0 plane.
Mesh makeQuad(float cx, float cy) {
    const std::vector<Vec3> p = {{cx - 0.5f, cy - 0.5f, 0.0f},
                                 {cx + 0.5f, cy - 0.5f, 0.0f},
                                 {cx + 0.5f, cy + 0.5f, 0.0f},
                                 {cx - 0.5f, cy + 0.5f, 0.0f}};
    const std::vector<std::vector<Index>> f = {{0, 1, 2, 3}};
    return Mesh::fromIndexed(p, f);
}

// Two quads as two disconnected components: far one first, near one second.
Mesh makeTwoQuads(float x0, float x1) {
    const std::vector<Vec3> p = {
        {x0 - 0.5f, -0.5f, 0.0f}, {x0 + 0.5f, -0.5f, 0.0f}, {x0 + 0.5f, 0.5f, 0.0f},
        {x0 - 0.5f, 0.5f, 0.0f},  {x1 - 0.5f, -0.5f, 0.0f}, {x1 + 0.5f, -0.5f, 0.0f},
        {x1 + 0.5f, 0.5f, 0.0f},  {x1 - 0.5f, 0.5f, 0.0f},
    };
    const std::vector<std::vector<Index>> f = {{0, 1, 2, 3}, {4, 5, 6, 7}};
    return Mesh::fromIndexed(p, f);
}

double distanceVariance(const bc::Cage& cage, const Mesh& mesh) {
    double sum = 0.0;
    double sumSq = 0.0;
    std::size_t n = 0;
    for (Index vi = 0; vi < static_cast<Index>(mesh.vertexCapacity()); ++vi) {
        if (!mesh.isAlive(VertexId{vi})) {
            continue;
        }
        const double d = static_cast<double>(cage.distance(VertexId{vi}));
        sum += d;
        sumSq += d * d;
        ++n;
    }
    const double mean = sum / static_cast<double>(n);
    return sumSq / static_cast<double>(n) - mean * mean;
}

}  // namespace

TEST_CASE("per-vertex override survives a falloff tweak of neighbours") {
    const Mesh mesh = makeGrid(3, 3);  // vertices 0..8, centre is vertex 4
    bc::Cage cage(mesh, 0.1f);

    const VertexId centre{4};
    cage.setVertexDistance(centre, 5.0f);
    REQUIRE(cage.isOverridden(centre));

    std::vector<VertexId> region;
    for (Index vi = 0; vi < static_cast<Index>(mesh.vertexCapacity()); ++vi) {
        region.push_back(VertexId{vi});
    }

    // Brush centred on the overridden vertex, comfortably covering its
    // neighbours, pushing the cage outward.
    const bc::Brush brush{mesh.position(centre), 2.5f, bc::Falloff::Linear};
    const std::size_t modified = cage.tweakDistance(region, 1.0f, brush);

    // The override is untouched by the falloff stroke...
    CHECK(cage.distance(centre) == doctest::Approx(5.0f));
    CHECK(cage.isOverridden(centre));
    // ...while at least one neighbour moved.
    CHECK(modified > 0);
    CHECK(cage.distance(VertexId{1}) > 0.1f);  // direct neighbour at distance 1
}

TEST_CASE("relax reduces distance variance") {
    const Mesh mesh = makeGrid(5, 5);
    bc::Cage cage(mesh, 0.0f);

    // Seed a noisy but unpinned distance field: set-then-clear leaves the value
    // in place without the override flag, so relax is free to smooth it.
    for (Index vi = 0; vi < static_cast<Index>(mesh.vertexCapacity()); ++vi) {
        const float d = (vi % 2 == 0) ? 2.0f : 0.0f;
        cage.setVertexDistance(VertexId{vi}, d);
        cage.clearOverride(VertexId{vi});
    }

    const double before = distanceVariance(cage, mesh);
    cage.relax(8, 0.5f);
    const double after = distanceVariance(cage, mesh);

    CHECK(before > 0.0);
    CHECK(after < before);
}

TEST_CASE("projection rays start outside the surface and aim inward") {
    const Mesh mesh = makeGrid(2, 2);
    bc::Cage cage(mesh, 0.5f);
    const auto rays = cage.projectionRays();
    REQUIRE(rays.size() == mesh.vertexCount());
    // Plane normal is +/-z; the ray sweep spans twice the cage distance.
    CHECK(rays[0].maxDistance == doctest::Approx(1.0f));
    CHECK(cyber::length(rays[0].direction) == doctest::Approx(1.0f));
}

TEST_CASE("cage state round-trips through serialize/deserialize") {
    const Mesh mesh = makeGrid(3, 3);
    bc::Cage cage(mesh, 0.1f);
    cage.setVertexDistance(VertexId{2}, 3.0f);
    const auto blob = cage.serialize();

    bc::Cage restored(mesh, 0.1f);
    REQUIRE(restored.deserialize(blob));
    CHECK(restored.distance(VertexId{2}) == doctest::Approx(3.0f));
    CHECK(restored.isOverridden(VertexId{2}));
}

TEST_CASE("link resolution returns the explicit link over nearest-surface") {
    // Edit component sits at the origin.
    const Mesh editMesh = makeQuad(0.0f, 0.0f);
    // Target: component 0 far (x = 10), component 1 near (x = 0).
    const Mesh targetMesh = makeTwoQuads(10.0f, 0.0f);

    bc::ComponentLinks linksModel(editMesh, targetMesh);
    REQUIRE(linksModel.editComponentCount() == 1);
    REQUIRE(linksModel.targetComponentCount() == 2);

    // Unlinked: nearest surface is the near component (index 1).
    const auto fallback = linksModel.resolveSource(0);
    CHECK_FALSE(fallback.explicitLink);
    REQUIRE(fallback.targets.size() == 1);
    CHECK(fallback.targets[0] == 1);

    // Explicitly link to the *far* component (index 0); it must win.
    linksModel.link(0, 0);
    const auto resolved = linksModel.resolveSource(0);
    CHECK(resolved.explicitLink);
    REQUIRE(resolved.targets.size() == 1);
    CHECK(resolved.targets[0] == 0);
}

TEST_CASE("bake-alone flag and preview descriptors") {
    const Mesh editMesh = makeQuad(0.0f, 0.0f);
    const Mesh targetMesh = makeQuad(0.0f, 0.0f);
    bc::ComponentLinks linksModel(editMesh, targetMesh);

    CHECK_FALSE(linksModel.bakeAlone(0));
    linksModel.setBakeAlone(0, true);
    CHECK(linksModel.bakeAlone(0));

    bc::BakePreview preview = bc::BakePreview::cageInspection();
    CHECK(preview.showCage);
    preview.light.moveBy(Vec3{0.0f, 0.0f, 1.0f});
    CHECK(preview.light.position.z == doctest::Approx(2.0f));
}
