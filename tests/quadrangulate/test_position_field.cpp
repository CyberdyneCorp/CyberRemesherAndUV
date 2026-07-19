#include <doctest.h>

#include <cmath>
#include <vector>

#include "cyber/core/math.hpp"
#include "cyber/core/mesh.hpp"
#include "cyber/quadrangulate/position_field.hpp"

using cyber::Index;
using cyber::Mesh;
using cyber::Vec3;
namespace remesh = cyber::remesh;

namespace {

// Open triangulated cylinder about +z. Its principal directions are the axis
// and the circumference, so a well-behaved 4-RoSy field aligns to them.
Mesh cylinder(float radius, float height, int rings, int segments) {
    std::vector<Vec3> p;
    std::vector<std::vector<Index>> f;
    const auto id = [&](int i, int j) { return static_cast<Index>(i * segments + (j % segments)); };
    for (int i = 0; i <= rings; ++i) {
        const float z = height * static_cast<float>(i) / static_cast<float>(rings);
        for (int j = 0; j < segments; ++j) {
            const float a = 2.0f * 3.14159265358979323846f * static_cast<float>(j) /
                            static_cast<float>(segments);
            p.push_back({radius * std::cos(a), radius * std::sin(a), z});
        }
    }
    for (int i = 0; i < rings; ++i) {
        for (int j = 0; j < segments; ++j) {
            f.push_back({id(i, j), id(i + 1, j), id(i + 1, j + 1)});
            f.push_back({id(i, j), id(i + 1, j + 1), id(i, j + 1)});
        }
    }
    return Mesh::fromIndexed(p, f);
}

// Closed UV sphere. Its two poles are 4-RoSy singularities, so the extractor's
// face walk produces n-gon orbits there — the regime that exercises the
// greedy-cut hole fill (fillFace), unlike the developable cylinder.
Mesh uvSphere(float radius, int rings, int segments) {
    std::vector<Vec3> p;
    std::vector<std::vector<Index>> f;
    const auto id = [&](int i, int j) { return static_cast<Index>(i * segments + (j % segments)); };
    for (int i = 0; i <= rings; ++i) {
        const float theta = 3.14159265358979323846f * static_cast<float>(i) / static_cast<float>(rings);
        for (int j = 0; j < segments; ++j) {
            const float phi = 2.0f * 3.14159265358979323846f * static_cast<float>(j) /
                              static_cast<float>(segments);
            p.push_back({radius * std::sin(theta) * std::cos(phi),
                         radius * std::sin(theta) * std::sin(phi), radius * std::cos(theta)});
        }
    }
    for (int i = 0; i < rings; ++i) {
        for (int j = 0; j < segments; ++j) {
            f.push_back({id(i, j), id(i + 1, j), id(i + 1, j + 1)});
            f.push_back({id(i, j), id(i + 1, j + 1), id(i, j + 1)});
        }
    }
    return Mesh::fromIndexed(p, f);
}

// Best 4-RoSy agreement between two tangent-plane directions about normal n.
float rosyAgreement(Vec3 a, Vec3 b, Vec3 n) {
    float best = -1.0f;
    for (int k = 0; k < 4; ++k) {
        best = std::fmax(best, dot(a, b));
        b = cross(n, b);  // rotate 90 degrees
    }
    return best;
}

}  // namespace

// The per-vertex 4-RoSy orientation field must be smooth (neighbouring crosses
// agree under 90-degree symmetry) and, on a cylinder, aligned to the principal
// directions (axis / circumference) — i.e. every vertex's cross is essentially
// axial or circumferential, never diagonal.
TEST_CASE("position field: orientation is smooth and principal-direction aligned") {
    const Mesh cyl = cylinder(1.0f, 3.0f, 40, 40);
    const remesh::PositionField field = remesh::computePositionField(cyl, 0.2f, 25);

    std::vector<Vec3> P;
    std::vector<std::vector<Index>> F;
    cyl.toIndexed(P, F);

    double agreeSum = 0.0;
    std::size_t edgeCount = 0;
    double alignSum = 0.0;
    std::size_t vertCount = 0;
    for (const auto& tri : F) {
        for (int k = 0; k < 3; ++k) {
            const std::size_t i = tri[static_cast<std::size_t>(k)];
            const std::size_t j = tri[static_cast<std::size_t>((k + 1) % 3)];
            if (!field.valid[i] || !field.valid[j]) {
                continue;
            }
            agreeSum += static_cast<double>(rosyAgreement(field.q[i], field.q[j], field.normal[i]));
            ++edgeCount;
        }
    }
    for (std::size_t i = 0; i < field.size(); ++i) {
        if (!field.valid[i]) {
            continue;
        }
        // Axis alignment: the cross's closer axis to the z axis should be nearly
        // parallel or nearly perpendicular, never at ~45 degrees. Measure how far
        // the field direction is from a principal axis: min over the 4 RoSy reps
        // of the angle to the z axis, folded into [0, 45] degrees; expect small.
        const Vec3 q = field.q[i];
        const Vec3 qp = field.qPerp(i);
        const float axial = std::fmax(std::fabs(q.z), std::fabs(qp.z));  // 1 = perfectly aligned
        alignSum += static_cast<double>(axial);
        ++vertCount;
    }

    REQUIRE(edgeCount > 0);
    REQUIRE(vertCount > 0);
    const double agreement = agreeSum / static_cast<double>(edgeCount);
    const double alignment = alignSum / static_cast<double>(vertCount);
    CAPTURE(agreement);
    CAPTURE(alignment);
    CHECK(agreement > 0.95);   // neighbouring crosses nearly identical under RoSy
    CHECK(alignment > 0.9);    // one field axis runs along the cylinder axis
}

// The wired position-field quadrangulator must turn a closed triangle mesh at
// ~target edge length into a valid, strongly quad-dominant mesh — the
// rotation-system walk + fan-split cover the whole surface (only singularity
// triangles remain), unlike the old ~30%-coverage walk.
TEST_CASE("instant-meshes quadrangulator gives a valid quad-dominant mesh") {
    Mesh cyl = cylinder(1.0f, 3.0f, 12, 25);  // ~unit edge length grid
    cyl.tagFeatureEdges(90.0f);
    auto quad = remesh::makeInstantMeshesQuadrangulator(25);
    const auto outcome = quad->quadrangulate(cyl, 0.25f, nullptr, nullptr);
    REQUIRE(outcome.success);
    REQUIRE(quad->name() == "instant-meshes");
    REQUIRE(cyl.validate().empty());

    std::vector<Vec3> P;
    std::vector<std::vector<Index>> F;
    cyl.toIndexed(P, F);
    std::size_t quads = 0;
    for (const auto& f : F) {
        if (f.size() == 4) {
            ++quads;
        }
    }
    CAPTURE(F.size());
    CAPTURE(quads);
    REQUIRE(F.size() > 0);
    CHECK(static_cast<double>(quads) / static_cast<double>(F.size()) > 0.9);
}

// The position field must stay on the surface near each vertex (it is a
// representative of that vertex's lattice cell, not a free point): every o_i is
// within a small multiple of the spacing of its vertex.
TEST_CASE("position field: representatives stay near their vertices") {
    const Mesh cyl = cylinder(1.0f, 3.0f, 30, 30);
    const float spacing = 0.2f;
    const remesh::PositionField field = remesh::computePositionField(cyl, spacing, 20);

    std::vector<Vec3> P;
    std::vector<std::vector<Index>> F;
    cyl.toIndexed(P, F);
    REQUIRE(P.size() == field.size());

    float maxDrift = 0.0f;
    for (std::size_t i = 0; i < field.size(); ++i) {
        if (field.valid[i]) {
            maxDrift = std::fmax(maxDrift, cyber::length(field.o[i] - P[i]));
        }
    }
    CAPTURE(maxDrift);
    CHECK(maxDrift < spacing);  // representative stays within a cell of its vertex
}

// Full extraction on a developable surface (a cylinder, no singularities) with
// the mesh already at ~target edge length — the regime the pipeline's isotropic
// stage produces. The position-field extraction must yield a valid, fully
// quad-dominant grid. (Surfaces with singularities need the Stage-C handling
// and are exercised separately once that lands.)
TEST_CASE("position field: extraction gives a clean quad grid on a cylinder") {
    const float spacing = 0.25f;
    const int rings = 12;     // height 3 / spacing
    const int segments = 25;  // circumference 2*pi / spacing
    const Mesh cyl = cylinder(1.0f, 3.0f, rings, segments);
    const remesh::PositionField field = remesh::computePositionField(cyl, spacing, 60);
    const Mesh quads = remesh::extractQuadMesh(cyl, field);

    std::vector<Vec3> P;
    std::vector<std::vector<Index>> F;
    quads.toIndexed(P, F);
    std::size_t quadCount = 0;
    for (const auto& f : F) {
        if (f.size() == 4) {
            ++quadCount;
        }
    }
    CAPTURE(F.size());
    CAPTURE(quadCount);
    REQUIRE(F.size() > 0);
    CHECK(quads.validate().empty());                                             // manifold
    CHECK(static_cast<double>(quadCount) / static_cast<double>(F.size()) > 0.90);  // quad grid
}

// Regression for the greedy-cut hole fill (fillFace). A UV sphere's poles are
// singularities, so extraction produces n-gon orbits there. The old centroid
// fan-split filled them with pie-slice quads (many corners well under 30 deg);
// greedy-cut instead emits near-90-deg quads from the orbit's own vertices. The
// result must stay manifold and quad-dominant, and only a small fraction of
// quads may have a sharp (< 30 deg) corner — a centroid fan would blow past this.
TEST_CASE("position field: greedy-cut hole fill avoids pie-slice slivers") {
    const Mesh sphere = uvSphere(1.0f, 24, 30);
    const remesh::PositionField field = remesh::computePositionField(sphere, 0.14f, 40);
    const Mesh quads = remesh::extractQuadMesh(sphere, field);

    std::vector<Vec3> P;
    std::vector<std::vector<Index>> F;
    quads.toIndexed(P, F);
    REQUIRE(F.size() > 0);
    CHECK(quads.validate().empty());  // manifold

    std::size_t quadCount = 0;
    std::size_t slivers = 0;  // quads with a corner < 30 deg
    for (const auto& f : F) {
        if (f.size() != 4) {
            continue;
        }
        ++quadCount;
        float worst = 180.0f;
        for (int k = 0; k < 4; ++k) {
            const Vec3 a = P[f[static_cast<std::size_t>(k)]];
            const Vec3 b = P[f[static_cast<std::size_t>((k + 1) % 4)]];
            const Vec3 pr = P[f[static_cast<std::size_t>((k + 3) % 4)]];
            const Vec3 e1 = b - a, e2 = pr - a;
            const float c = dot(e1, e2) / (length(e1) * length(e2) + 1e-6f);
            worst = std::fmin(worst, std::acos(std::clamp(c, -1.0f, 1.0f)) * 180.0f / 3.14159265f);
        }
        if (worst < 30.0f) {
            ++slivers;
        }
    }
    REQUIRE(quadCount > 0);
    const double quadFrac = static_cast<double>(quadCount) / static_cast<double>(F.size());
    const double sliverFrac = static_cast<double>(slivers) / static_cast<double>(quadCount);
    CAPTURE(F.size());
    CAPTURE(quadFrac);
    CAPTURE(sliverFrac);
    CHECK(quadFrac > 0.75);       // strongly quad-dominant despite the two poles
    CHECK(sliverFrac < 0.10);     // greedy-cut keeps pie-slice slivers rare
}
