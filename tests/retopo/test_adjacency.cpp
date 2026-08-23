// The flat one-ring / vertex->face table the brush sweeps read instead of
// gathering their neighbourhood per vertex. Everything here pins BEHAVIOUR: the
// table must answer exactly what the mesh queries answer, in the same order,
// and a sweep handed the table must produce bit-identical geometry to one that
// is not. Speed is the reason the table exists, but nothing below times
// anything.
#include <doctest.h>

#include <cstddef>
#include <cstring>
#include <vector>

#include "cyber/core/mesh.hpp"
#include "cyber/retopo/adjacency.hpp"
#include "cyber/retopo/neighbors.hpp"
#include "cyber/retopo/relax.hpp"
#include "cyber/retopo/soft_selection.hpp"

using cyber::FaceId;
using cyber::Index;
using cyber::Mesh;
using cyber::Vec3;
using cyber::VertexId;
namespace retopo = cyber::retopo;

namespace {

// A cols x rows quad grid in the plane z = 0, with an interior bump so the
// vertex normals are not all identical.
Mesh makeGrid(int cols, int rows) {
    std::vector<Vec3> p;
    for (int j = 0; j < rows; ++j) {
        for (int i = 0; i < cols; ++i) {
            const float x = static_cast<float>(i);
            const float y = static_cast<float>(j);
            const float z = ((i + j) % 3 == 0) ? 0.25f : 0.0f;
            p.push_back(Vec3{x, y, z});
        }
    }
    std::vector<std::vector<Index>> f;
    for (int j = 0; j + 1 < rows; ++j) {
        for (int i = 0; i + 1 < cols; ++i) {
            const Index a = static_cast<Index>(j * cols + i);
            const Index stride = static_cast<Index>(cols);
            f.push_back({a, a + 1, a + stride + 1, a + stride});
        }
    }
    return Mesh::fromIndexed(p, f);
}

// Bit-exact comparison: these are identity checks, not tolerance checks.
bool sameBits(const std::vector<Vec3>& a, const std::vector<Vec3>& b) {
    return a.size() == b.size() &&
           (a.empty() || std::memcmp(a.data(), b.data(), a.size() * sizeof(Vec3)) == 0);
}

std::vector<Vec3> positionsOf(const Mesh& mesh) {
    std::vector<Vec3> out;
    for (Index i = 0; i < mesh.vertexCapacity(); ++i) {
        const VertexId v{i};
        out.push_back(mesh.isAlive(v) ? mesh.position(v) : Vec3{});
    }
    return out;
}

std::vector<float> weightsOf(const retopo::SoftSelection& selection) {
    const std::span<const float> w = selection.weights();
    return std::vector<float>(w.begin(), w.end());
}

// A repeatable weight field, so two runs of a sweep start from the same state.
retopo::SoftSelection seedSelection(const Mesh& mesh) {
    retopo::SoftSelection selection;
    selection.resizeFor(mesh);
    for (Index i = 0; i < mesh.vertexCapacity(); ++i) {
        const VertexId v{i};
        if (mesh.isAlive(v)) {
            selection.setWeight(v, static_cast<float>(i % 5) * 0.25f);
        }
    }
    return selection;
}

}  // namespace

TEST_CASE("adjacency table answers exactly what the mesh queries answer") {
    Mesh mesh = makeGrid(6, 5);
    retopo::MeshAdjacency adjacency;
    adjacency.build(mesh);

    for (Index i = 0; i < mesh.vertexCapacity(); ++i) {
        const VertexId v{i};
        if (!mesh.isAlive(v)) {
            continue;
        }
        const std::vector<VertexId> ring = retopo::oneRing(mesh, v);
        const std::span<const VertexId> cached = adjacency.ring(v);
        REQUIRE(cached.size() == ring.size());
        for (std::size_t k = 0; k < ring.size(); ++k) {
            CHECK(cached[k] == ring[k]);  // insertion order, not just the set
        }
        const std::vector<FaceId> faces = mesh.vertexFaces(v);
        const std::span<const FaceId> cachedFaces = adjacency.faces(v);
        REQUIRE(cachedFaces.size() == faces.size());
        for (std::size_t k = 0; k < faces.size(); ++k) {
            CHECK(cachedFaces[k] == faces[k]);  // ascending face id
        }
    }
}

// Deleting faces frees vertex, edge and face ids and leaves holes in the id
// space. The table must describe the survivors and hand back nothing for the
// dead, because a sweep indexes it by raw id.
TEST_CASE("adjacency table survives a mesh full of dead ids") {
    Mesh mesh = makeGrid(5, 5);
    mesh.removeFace(FaceId{0});
    mesh.removeFace(FaceId{3});
    mesh.removeFace(FaceId{7});
    // The corner quad's outer vertex is left with no edges at all; retiring it
    // is what punches a hole in the vertex id space.
    REQUIRE(mesh.removeIsolatedVertex(VertexId{0}));

    retopo::MeshAdjacency adjacency;
    adjacency.build(mesh);
    std::size_t deadVertices = 0;
    for (Index i = 0; i < mesh.vertexCapacity(); ++i) {
        const VertexId v{i};
        if (!mesh.isAlive(v)) {
            ++deadVertices;
            CHECK(adjacency.ring(v).empty());
            CHECK(adjacency.faces(v).empty());
            continue;
        }
        CHECK(adjacency.ring(v).size() == retopo::oneRing(mesh, v).size());
        CHECK(adjacency.faces(v).size() == mesh.vertexFaces(v).size());
    }
    CHECK(deadVertices > 0u);  // the corner really was freed

    // An id past the table is empty rather than out of bounds.
    CHECK(adjacency.ring(VertexId{static_cast<Index>(mesh.vertexCapacity() + 10)}).empty());
    CHECK(adjacency.faces(VertexId{static_cast<Index>(mesh.vertexCapacity() + 10)}).empty());
}

// The whole point of the table is that a lookup reads stored data instead of
// regenerating it, so two lookups of the same vertex must return the SAME
// storage, not two equal copies.
TEST_CASE("adjacency lookups return stored spans, not per-call gathers") {
    Mesh mesh = makeGrid(4, 4);
    retopo::MeshAdjacency adjacency;
    adjacency.build(mesh);
    const VertexId v{5};
    REQUIRE(!adjacency.ring(v).empty());
    CHECK(adjacency.ring(v).data() == adjacency.ring(v).data());
    CHECK(adjacency.faces(v).data() == adjacency.faces(v).data());
}

TEST_CASE("a cleared or outgrown table is not offered to a sweep") {
    Mesh mesh = makeGrid(4, 4);
    retopo::MeshAdjacency adjacency;
    CHECK(!adjacency.built());
    CHECK(!adjacency.covers(mesh));

    adjacency.build(mesh);
    CHECK(adjacency.built());
    CHECK(adjacency.covers(mesh));

    mesh.addVertex(Vec3{9.0f, 9.0f, 0.0f});  // the mesh grew under the table
    CHECK(!adjacency.covers(mesh));

    adjacency.clear();
    CHECK(!adjacency.built());
}

// Identity is the contract: a sweep must not care whether the caller had a
// table. Same neighbours, same order, same sums, same bits.
TEST_CASE("relax is bit-identical with and without the adjacency table") {
    const Mesh source = makeGrid(7, 6);
    retopo::RelaxParams params;
    params.strength = 0.4f;
    params.iterations = 3;

    Mesh plain = source;
    retopo::relax(plain, params);

    Mesh cached = source;
    retopo::MeshAdjacency adjacency;
    adjacency.build(cached);
    retopo::relax(cached, params, nullptr, nullptr, &adjacency);

    CHECK(sameBits(positionsOf(plain), positionsOf(cached)));
    CHECK(!sameBits(positionsOf(plain), positionsOf(source)));  // it did move things
}

TEST_CASE("weighted relax is bit-identical with and without the adjacency table") {
    const Mesh source = makeGrid(7, 6);
    retopo::RelaxParams params;
    params.strength = 0.6f;
    params.iterations = 4;

    Mesh plain = source;
    const retopo::SoftSelection plainSelection = seedSelection(plain);
    const retopo::ResnapReport plainReport = retopo::relaxWeighted(plain, plainSelection, params);

    Mesh cached = source;
    const retopo::SoftSelection cachedSelection = seedSelection(cached);
    retopo::MeshAdjacency adjacency;
    adjacency.build(cached);
    const retopo::ResnapReport cachedReport =
        retopo::relaxWeighted(cached, cachedSelection, params, nullptr, nullptr, 0.0f, &adjacency);

    CHECK(sameBits(positionsOf(plain), positionsOf(cached)));
    CHECK(plainReport.moved == cachedReport.moved);
    CHECK(plainReport.resnapped == cachedReport.resnapped);
}

TEST_CASE("selection sweeps are bit-identical with and without the adjacency table") {
    const Mesh mesh = makeGrid(7, 6);
    retopo::MeshAdjacency adjacency;
    adjacency.build(mesh);

    for (int steps : {1, 3, 10}) {
        retopo::SoftSelection plain = seedSelection(mesh);
        retopo::SoftSelection cached = seedSelection(mesh);
        retopo::smoothSelection(mesh, plain, steps);
        retopo::smoothSelection(mesh, cached, steps, &adjacency);
        CHECK(weightsOf(plain) == weightsOf(cached));

        retopo::SoftSelection grownPlain = seedSelection(mesh);
        retopo::SoftSelection grownCached = seedSelection(mesh);
        retopo::expandSelection(mesh, grownPlain, steps);
        retopo::expandSelection(mesh, grownCached, steps, &adjacency);
        CHECK(weightsOf(grownPlain) == weightsOf(grownCached));

        retopo::SoftSelection shrunkPlain = seedSelection(mesh);
        retopo::SoftSelection shrunkCached = seedSelection(mesh);
        retopo::contractSelection(mesh, shrunkPlain, steps);
        retopo::contractSelection(mesh, shrunkCached, steps, &adjacency);
        CHECK(weightsOf(shrunkPlain) == weightsOf(shrunkCached));
    }
}

// A table built before a topology change describes a mesh that no longer
// exists. Handing it to a sweep anyway must not produce the wrong answer: the
// size guard rejects it and the sweep falls back to the mesh.
TEST_CASE("a sweep handed a table from a different mesh still relaxes correctly") {
    const Mesh source = makeGrid(6, 5);

    Mesh mesh = source;
    retopo::MeshAdjacency stale;
    stale.build(mesh);
    mesh.addVertex(Vec3{20.0f, 20.0f, 0.0f});  // capacity changes, table does not

    retopo::RelaxParams params;
    params.strength = 0.5f;
    params.iterations = 2;

    Mesh reference = mesh;
    retopo::relax(reference, params);
    retopo::relax(mesh, params, nullptr, nullptr, &stale);
    CHECK(sameBits(positionsOf(reference), positionsOf(mesh)));
}

// The per-sweep face-normal memo is only sound because a relax iteration reads
// everything before it writes anything. That makes reset() a hard requirement
// between iterations, so pin both halves of the contract.
TEST_CASE("the face-normal memo answers like the mesh and clears on reset") {
    Mesh mesh = makeGrid(4, 4);
    retopo::detail::FaceNormalCache normals(mesh);
    normals.reset();

    const FaceId face{0};
    const Vec3 first = normals.normal(face);
    const Vec3 direct = mesh.faceNormal(face);
    CHECK(first.x == direct.x);
    CHECK(first.y == direct.y);
    CHECK(first.z == direct.z);
    CHECK(normals.normal(face).x == first.x);  // memoized, same answer

    // Tilt the face. Until reset() the memo deliberately keeps the value the
    // iteration started with; after it, the new geometry shows through.
    const VertexId corner = mesh.faceVertices(face).front();
    mesh.setPosition(corner, mesh.position(corner) + Vec3{0.0f, 0.0f, 3.0f});
    CHECK(normals.normal(face).z == first.z);
    normals.reset();
    const Vec3 moved = mesh.faceNormal(face);
    CHECK(normals.normal(face).x == moved.x);
    CHECK(normals.normal(face).z == moved.z);
}
