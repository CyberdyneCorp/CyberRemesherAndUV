#include <doctest.h>

#include <array>
#include <cstddef>
#include <cstdint>
#include <map>
#include <utility>
#include <vector>

#include "cyber/core/mesh.hpp"
#include "cyber/core/quadrangulate.hpp"
#include "cyber/quadrangulate/field_quadrangulator.hpp"

using cyber::EdgeId;
using cyber::FaceId;
using cyber::Index;
using cyber::Mesh;
using cyber::Vec3;
using cyber::VertexId;
namespace remesh = cyber::remesh;

namespace {

std::size_t quadCount(const Mesh& mesh) {
    std::size_t q = 0;
    for (Index i = 0; i < mesh.faceCapacity(); ++i) {
        if (mesh.isAlive(FaceId{i}) && mesh.faceSize(FaceId{i}) == 4) {
            ++q;
        }
    }
    return q;
}

// Mean |valence - 4| over vertices that are interior (only manifold,
// non-feature incident edges) and quad-only (every incident face a quad) — the
// exact population the cleanup is allowed to touch.
double meanInteriorQuadDeviation(const Mesh& mesh) {
    double sum = 0.0;
    std::size_t count = 0;
    for (Index i = 0; i < mesh.vertexCapacity(); ++i) {
        const VertexId v{i};
        if (!mesh.isAlive(v)) {
            continue;
        }
        bool eligible = true;
        for (const EdgeId e : mesh.vertexEdges(v)) {
            if (mesh.edgeFaceCount(e) != 2 || mesh.isFeatureEdge(e)) {
                eligible = false;
                break;
            }
        }
        if (eligible) {
            for (const FaceId f : mesh.vertexFaces(v)) {
                if (mesh.faceSize(f) != 4) {
                    eligible = false;
                    break;
                }
            }
        }
        if (!eligible) {
            continue;
        }
        const int val = static_cast<int>(mesh.vertexEdges(v).size());
        const int d = val - 4;
        sum += static_cast<double>(d < 0 ? -d : d);
        ++count;
    }
    return count == 0 ? 0.0 : sum / static_cast<double>(count);
}

// Icosphere: icosahedron refined `subdivisions` times, projected to the unit
// sphere. A closed, all-triangle, boundary-free mesh — the classic test island
// for a quadrangulator.
Mesh makeIcosphere(int subdivisions) {
    const float t = 1.61803398875f;  // golden ratio
    const float o = 1.0f, z = 0.0f;
    std::vector<Vec3> pos = {
        {-o, t, z},  {o, t, z},  {-o, -t, z}, {o, -t, z}, {z, -o, t},  {z, o, t},
        {z, -o, -t}, {z, o, -t}, {t, z, -o},  {t, z, o},  {-t, z, -o}, {-t, z, o},
    };
    std::vector<std::array<Index, 3>> tris = {
        {0, 11, 5}, {0, 5, 1}, {0, 1, 7}, {0, 7, 10}, {0, 10, 11},
        {1, 5, 9}, {5, 11, 4}, {11, 10, 2}, {10, 7, 6}, {7, 1, 8},
        {3, 9, 4}, {3, 4, 2}, {3, 2, 6}, {3, 6, 8}, {3, 8, 9},
        {4, 9, 5}, {2, 4, 11}, {6, 2, 10}, {8, 6, 7}, {9, 8, 1},
    };
    for (int s = 0; s < subdivisions; ++s) {
        std::map<std::pair<Index, Index>, Index> midCache;
        const auto midpoint = [&](Index a, Index b) {
            const auto key = a < b ? std::make_pair(a, b) : std::make_pair(b, a);
            const auto it = midCache.find(key);
            if (it != midCache.end()) {
                return it->second;
            }
            const Vec3 m = (pos[a] + pos[b]) * 0.5f;
            const Index id = static_cast<Index>(pos.size());
            pos.push_back(m);
            midCache.emplace(key, id);
            return id;
        };
        std::vector<std::array<Index, 3>> next;
        next.reserve(tris.size() * 4);
        for (const auto& tri : tris) {
            const Index a = midpoint(tri[0], tri[1]);
            const Index b = midpoint(tri[1], tri[2]);
            const Index c = midpoint(tri[2], tri[0]);
            next.push_back({tri[0], a, c});
            next.push_back({tri[1], b, a});
            next.push_back({tri[2], c, b});
            next.push_back({a, b, c});
        }
        tris = std::move(next);
    }
    for (Vec3& p : pos) {
        p = normalized(p);
    }
    std::vector<std::vector<Index>> faces;
    faces.reserve(tris.size());
    for (const auto& tri : tris) {
        faces.push_back({tri[0], tri[1], tri[2]});
    }
    return Mesh::fromIndexed(pos, faces);
}

}  // namespace

TEST_CASE("valence cleanup on a field-aligned sphere never worsens the mesh") {
    Mesh mesh = makeIcosphere(2);
    mesh.tagFeatureEdges(45.0f);  // smooth closed sphere: nothing tagged

    auto quad = remesh::makeFieldAlignedQuadrangulator();
    const auto outcome = quad->quadrangulate(mesh, 0.5f, nullptr, nullptr);
    REQUIRE(outcome.success);
    REQUIRE(mesh.validate().empty());

    // The quadrangulator already ran the cleanup; a second pass must be a safe
    // fixed point: never increase the deviation, keep the mesh valid, and leave
    // the quad count untouched.
    const double before = meanInteriorQuadDeviation(mesh);
    const std::size_t quadsBefore = quadCount(mesh);
    remesh::quadValenceCleanup(mesh);
    const double after = meanInteriorQuadDeviation(mesh);

    CHECK(mesh.validate().empty());
    CHECK(quadCount(mesh) == quadsBefore);
    CHECK(after <= before + 1e-9);
}

TEST_CASE("valence cleanup rotates a seeded defect back to all-valence-4") {
    // A 4-row x 5-col planar quad grid (12 quads). Every interior vertex is
    // valence 4. We overwrite the two central quads with their rotated pair,
    // seeding a valence {5,5,3,3} defect (edge 7-12 -> edge 8-11), then check
    // the cleanup rotates it back so every interior vertex is valence 4 again.
    const int cols = 5, rows = 4;
    const auto idx = [cols](int r, int c) { return static_cast<Index>(r * cols + c); };

    std::vector<Vec3> pos;
    for (int r = 0; r < rows; ++r) {
        for (int c = 0; c < cols; ++c) {
            pos.push_back({static_cast<float>(c), static_cast<float>(r), 0.0f});
        }
    }
    std::vector<std::vector<Index>> faces;
    for (int r = 0; r < rows - 1; ++r) {
        for (int c = 0; c < cols - 1; ++c) {
            if (r == 1 && (c == 1 || c == 2)) {
                continue;  // leave a hole for the seeded rotated pair
            }
            faces.push_back({idx(r, c), idx(r, c + 1), idx(r + 1, c + 1), idx(r + 1, c)});
        }
    }
    // Rotated replacement of quads (6,7,12,11) and (7,8,13,12): the shared
    // diagonal is 8-11 instead of 7-12, so vertices 7 and 12 drop to valence 3
    // and 8 and 11 rise to valence 5.
    faces.push_back({11, 6, 7, 8});
    faces.push_back({8, 13, 12, 11});

    Mesh mesh = Mesh::fromIndexed(pos, faces);
    REQUIRE(mesh.validate().empty());
    const std::size_t quadsBefore = quadCount(mesh);
    REQUIRE(quadsBefore == 12);

    const double before = meanInteriorQuadDeviation(mesh);
    REQUIRE(before > 0.0);  // the seeded defect is measurable

    const std::size_t applied = remesh::quadValenceCleanup(mesh);
    const double after = meanInteriorQuadDeviation(mesh);

    CHECK(applied >= 1);                    // a rotation actually fired
    CHECK(mesh.validate().empty());         // still a valid manifold mesh
    CHECK(quadCount(mesh) == quadsBefore);  // quad count preserved
    CHECK(after < before);                  // deviation strictly decreased
    CHECK(after == doctest::Approx(0.0));   // fully repaired to valence 4
}

TEST_CASE("valence cleanup on an already-regular grid is a no-op") {
    // A clean 5x5 vertex grid (16 quads, all interior vertices valence 4): the
    // cleanup must find nothing to do and leave the mesh bit-for-bit valid.
    const int n = 5;
    const auto idx = [n](int r, int c) { return static_cast<Index>(r * n + c); };
    std::vector<Vec3> pos;
    for (int r = 0; r < n; ++r) {
        for (int c = 0; c < n; ++c) {
            pos.push_back({static_cast<float>(c), static_cast<float>(r), 0.0f});
        }
    }
    std::vector<std::vector<Index>> faces;
    for (int r = 0; r < n - 1; ++r) {
        for (int c = 0; c < n - 1; ++c) {
            faces.push_back({idx(r, c), idx(r, c + 1), idx(r + 1, c + 1), idx(r + 1, c)});
        }
    }
    Mesh mesh = Mesh::fromIndexed(pos, faces);
    const std::size_t quadsBefore = quadCount(mesh);
    const double before = meanInteriorQuadDeviation(mesh);

    const std::size_t applied = remesh::quadValenceCleanup(mesh);

    CHECK(applied == 0);
    CHECK(mesh.validate().empty());
    CHECK(quadCount(mesh) == quadsBefore);
    CHECK(meanInteriorQuadDeviation(mesh) == doctest::Approx(before));
}
