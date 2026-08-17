#include <doctest.h>

#include <cmath>
#include <cstddef>
#include <optional>
#include <random>
#include <vector>

#include "cyber/core/mesh.hpp"
#include "cyber/retopo/snapping.hpp"

using cyber::Index;
using cyber::Mesh;
using cyber::Vec3;
using cyber::VertexId;
namespace retopo = cyber::retopo;

// SurfaceSnapper::snapToVertex used to be a full scan of the Target's vertex
// table on EVERY call: 1.9 ms per single query on a 4.4M-triangle Target, which
// no interactive frame budget survives. It now descends a hierarchy over the
// vertices instead. That is a pure performance change, so what these cases pin
// is the ANSWER: the accelerated query must return exactly what the scan did,
// bit for bit, including which vertex wins when two are equidistant.
namespace {

// The literal pre-change implementation, kept here as the oracle.
std::optional<retopo::VertexHit> scanForNearest(const Mesh& mesh, Vec3 query, float radius) {
    const float radiusSquared = radius * radius;
    std::optional<retopo::VertexHit> best;
    for (Index i = 0; i < mesh.vertexCapacity(); ++i) {
        const VertexId v{i};
        if (!mesh.isAlive(v)) {
            continue;
        }
        const Vec3 position = mesh.position(v);
        const float d2 = lengthSquared(position - query);
        if (d2 > radiusSquared) {
            continue;
        }
        if (!best || d2 < best->distanceSquared) {
            best = retopo::VertexHit{v, position, d2};
        }
    }
    return best;
}

void checkMatchesScan(const Mesh& mesh, const retopo::SurfaceSnapper& snap, Vec3 query,
                      float radius) {
    const std::optional<retopo::VertexHit> want = scanForNearest(mesh, query, radius);
    const std::optional<retopo::VertexHit> got = snap.snapToVertex(query, radius);
    REQUIRE(want.has_value() == got.has_value());
    if (!want) {
        return;
    }
    CHECK(got->vertex.value == want->vertex.value);
    CHECK(got->distanceSquared == want->distanceSquared);  // bitwise, not Approx
    CHECK(got->point.x == want->point.x);
    CHECK(got->point.y == want->point.y);
    CHECK(got->point.z == want->point.z);
}

// A grid of quads on a bumpy surface: enough vertices for the hierarchy to have
// real depth, with the regular spacing that makes exact ties commonplace.
Mesh gridSurface(int n) {
    std::vector<Vec3> positions;
    for (int j = 0; j <= n; ++j) {
        for (int i = 0; i <= n; ++i) {
            const float x = static_cast<float>(i) / static_cast<float>(n) - 0.5f;
            const float y = static_cast<float>(j) / static_cast<float>(n) - 0.5f;
            positions.push_back({x, y, 0.1f * std::sin(x * 9.0f) * std::cos(y * 7.0f)});
        }
    }
    std::vector<std::vector<Index>> faces;
    const auto stride = static_cast<Index>(n + 1);
    for (int j = 0; j < n; ++j) {
        for (int i = 0; i < n; ++i) {
            const auto a = static_cast<Index>(j * (n + 1) + i);
            faces.push_back({a, a + 1, a + stride + 1, a + stride});
        }
    }
    return Mesh::fromIndexed(positions, faces);
}

}  // namespace

TEST_CASE("snapToVertex reproduces the scan exactly (spec: manual-retopology)") {
    const Mesh target = gridSurface(40);
    const retopo::SurfaceSnapper snap(target);
    REQUIRE(target.vertexCount() == 1681);

    // The query is answered from a hierarchy, not by walking the vertex table —
    // the whole point of the change, and invisible in the answers alone.
    CHECK(snap.vertexNodeCount() > 1);

    std::mt19937 rng(4242);
    std::uniform_real_distribution<float> coord(-0.7f, 0.7f);
    // Radii spanning "nothing in range", "a handful in range" and "everything".
    const std::vector<float> radii = {0.0f, 0.004f, 0.02f, 0.1f, 0.5f, 1e30f};
    for (int i = 0; i < 400; ++i) {
        const Vec3 query{coord(rng), coord(rng), coord(rng) * 0.3f};
        for (const float radius : radii) {
            CAPTURE(radius);
            checkMatchesScan(target, snap, query, radius);
        }
    }

    // Queries sitting exactly ON a Target vertex: distance 0 to that vertex and
    // the tightest possible pruning bound, which is where a hierarchy is most
    // likely to prune away the answer the scan would have found.
    for (Index i = 0; i < target.vertexCapacity(); ++i) {
        const VertexId v{i};
        if (!target.isAlive(v)) {
            continue;
        }
        for (const float radius : radii) {
            checkMatchesScan(target, snap, target.position(v), radius);
        }
    }
}

TEST_CASE("snapToVertex breaks an exact tie the way the scan did") {
    // Four vertices exactly equidistant from the origin. The scan ran in id
    // order and kept the first, so the LOWEST id wins; the hierarchy visits them
    // in whatever order its boxes imply, and must still name that one.
    const std::vector<Vec3> p = {{1, 0, 0}, {-1, 0, 0}, {0, 1, 0}, {0, -1, 0}, {0, 0, 4}};
    const std::vector<std::vector<Index>> f = {{0, 2, 4}, {2, 1, 4}, {1, 3, 4}, {3, 0, 4}};
    const Mesh target = Mesh::fromIndexed(p, f);
    const retopo::SurfaceSnapper snap(target);

    const auto hit = snap.snapToVertex({0, 0, 0}, 2.0f);
    REQUIRE(hit.has_value());
    CHECK(hit->vertex.value == 0);
    CHECK(hit->distanceSquared == 1.0f);
    checkMatchesScan(target, snap, {0, 0, 0}, 2.0f);

    // The radius bound is INCLUSIVE, as the scan's `d2 > radiusSquared` skip was.
    CHECK(snap.snapToVertex({0, 0, 0}, 1.0f).has_value());
    CHECK_FALSE(snap.snapToVertex({0, 0, 0}, 0.9f).has_value());

    // A negative radius squares to a positive bound — preserved, not clamped.
    CHECK(snap.snapToVertex({0, 0, 0}, -1.0f).has_value());
}

TEST_CASE("an equidistant vertex in the subtree visited second still wins on id") {
    // The tie above sits in a single leaf, so it says nothing about pruning.
    // Here the two equidistant vertices are in DIFFERENT subtrees whose boxes
    // are at exactly the same distance from the query, and the one with the
    // lower id — the one a scan in id order would keep — is in the subtree the
    // traversal reaches SECOND. Pruning at `>=` the incumbent distance, or
    // dropping the id tie-break, both answer with the other vertex.
    //
    // Coordinates are exact binary fractions so "equidistant" is exact.
    std::vector<Vec3> p;
    p.push_back({1, 0, 0});  // id 0: the answer, in the far subtree
    for (int k = 1; k <= 4; ++k) {
        p.push_back({2, static_cast<float>(k) * 0.25f, 0});
        p.push_back({2, static_cast<float>(-k) * 0.25f, 0});
    }
    const auto mirrored = static_cast<Index>(p.size());
    p.push_back({-1, 0, 0});  // id 9: same distance, in the near subtree
    for (int k = 1; k <= 4; ++k) {
        p.push_back({-2, static_cast<float>(k) * 0.25f, 0});
        p.push_back({-2, static_cast<float>(-k) * 0.25f, 0});
    }
    REQUIRE(p.size() == 2 * static_cast<std::size_t>(mirrored));

    const Mesh target = Mesh::fromIndexed(p, {});
    const retopo::SurfaceSnapper snap(target);
    REQUIRE(snap.vertexNodeCount() > 1);  // it really did split

    const auto hit = snap.snapToVertex({0, 0, 0}, 1.5f);
    REQUIRE(hit.has_value());
    CHECK(hit->distanceSquared == 1.0f);
    CHECK(hit->vertex.value == 0);
    checkMatchesScan(target, snap, {0, 0, 0}, 1.5f);
    checkMatchesScan(target, snap, {0, 0, 0}, 1.0f);
}

TEST_CASE("snapToVertex handles degenerate targets") {
    const retopo::SurfaceSnapper none;
    CHECK(none.empty());
    CHECK_FALSE(none.snapToVertex({0, 0, 0}, 1e30f).has_value());

    // Every vertex on one spot: no axis has any spread, so the build has no
    // split to make and must still terminate and answer.
    std::vector<Vec3> p(64, Vec3{0.5f, 0.5f, 0.5f});
    std::vector<std::vector<Index>> f;
    for (Index i = 0; i + 2 < 64; ++i) {
        f.push_back({i, static_cast<Index>(i + 1), static_cast<Index>(i + 2)});
    }
    const Mesh coincident = Mesh::fromIndexed(p, f);
    if (coincident.vertexCount() > 0) {
        const retopo::SurfaceSnapper snap(coincident);
        checkMatchesScan(coincident, snap, {0.5f, 0.5f, 0.5f}, 1.0f);
        checkMatchesScan(coincident, snap, {9.0f, 9.0f, 9.0f}, 1.0f);
    }
}
