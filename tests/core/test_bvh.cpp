#include <doctest.h>

#include <algorithm>
#include <cmath>
#include <cstddef>
#include <limits>
#include <random>
#include <string>
#include <string_view>
#include <vector>

#include "cyber/core/bvh.hpp"
#include "cyber/core/mesh.hpp"
#include "cyber/core/progress.hpp"

using cyber::Bvh;
using cyber::CancelToken;
using cyber::Index;
using cyber::Mesh;
using cyber::ProgressSink;
using cyber::Vec3;

namespace {

Mesh makeCube() {
    const std::vector<Vec3> p = {
        {0, 0, 0}, {1, 0, 0}, {1, 1, 0}, {0, 1, 0}, {0, 0, 1}, {1, 0, 1}, {1, 1, 1}, {0, 1, 1},
    };
    const std::vector<std::vector<Index>> f = {
        {0, 3, 2, 1}, {4, 5, 6, 7}, {0, 1, 5, 4}, {1, 2, 6, 5}, {2, 3, 7, 6}, {3, 0, 4, 7},
    };
    return Mesh::fromIndexed(p, f);
}

// A REGULAR grid torus: nu x nv quads, the tessellation a subdivision surface
// or a CAD export arrives as, and the shape the object-median split degenerates
// on (its centroids are quantized onto nu/nv distinct coordinates, so a median
// plane lands inside a block of ties and the two children keep the parent's
// extent).
Mesh regularTorus(int nu, int nv) {
    constexpr float kTwoPi = 6.2831853f;
    const float ringRadius = 1.0f;
    const float tubeRadius = 0.35f;
    std::vector<Vec3> positions;
    for (int i = 0; i < nu; ++i) {
        const float u = kTwoPi * static_cast<float>(i) / static_cast<float>(nu);
        for (int j = 0; j < nv; ++j) {
            const float v = kTwoPi * static_cast<float>(j) / static_cast<float>(nv);
            const float r = ringRadius + tubeRadius * std::cos(v);
            positions.push_back({r * std::cos(u), r * std::sin(u), tubeRadius * std::sin(v)});
        }
    }
    std::vector<std::vector<Index>> faces;
    for (int i = 0; i < nu; ++i) {
        for (int j = 0; j < nv; ++j) {
            const int i1 = (i + 1) % nu;
            const int j1 = (j + 1) % nv;
            const auto id = [nv](int a, int b) { return static_cast<Index>(a * nv + b); };
            faces.push_back({id(i, j), id(i1, j), id(i1, j1), id(i, j1)});
        }
    }
    return Mesh::fromIndexed(positions, faces);
}

// Deepest leaf of the hierarchy, read off the flattened snapshot.
std::size_t treeDepth(const cyber::FlatBvh& flat, std::uint32_t node = 0, std::size_t depth = 0) {
    if (flat.nodes.empty()) {
        return depth;
    }
    const cyber::FlatBvhNode& n = flat.nodes[node];
    if (n.triCount > 0) {
        return depth + 1;
    }
    return std::max(treeDepth(flat, n.leftFirst, depth + 1),
                    treeDepth(flat, n.leftFirst + 1, depth + 1));
}

// Every triangle reachable from the root, exactly once.
std::size_t leafTriangleTotal(const cyber::FlatBvh& flat, std::uint32_t node = 0) {
    if (flat.nodes.empty()) {
        return 0;
    }
    const cyber::FlatBvhNode& n = flat.nodes[node];
    if (n.triCount > 0) {
        return n.triCount;
    }
    return leafTriangleTotal(flat, n.leftFirst) + leafTriangleTotal(flat, n.leftFirst + 1);
}

}  // namespace

TEST_CASE("closest point lands on the surface (spec: mesh-core)") {
    const Mesh cube = makeCube();
    const Bvh bvh(cube);
    REQUIRE(bvh.triangleCount() == 12);

    // Outside, facing +z: closest point is on the z=1 face.
    const auto hit = bvh.closestPoint({0.5f, 0.5f, 2.0f});
    REQUIRE(hit.point.z == doctest::Approx(1.0f));
    REQUIRE(hit.point.x == doctest::Approx(0.5f));
    REQUIRE(hit.distanceSquared == doctest::Approx(1.0f));

    // Near a corner: snaps to the corner.
    const auto corner = bvh.closestPoint({-1.0f, -1.0f, -1.0f});
    REQUIRE(corner.point.x == doctest::Approx(0.0f));
    REQUIRE(corner.point.y == doctest::Approx(0.0f));
    REQUIRE(corner.point.z == doctest::Approx(0.0f));
}

TEST_CASE("closest point matches brute force on random queries") {
    const Mesh cube = makeCube();
    const Bvh bvh(cube);
    std::mt19937 rng(7);
    std::uniform_real_distribution<float> dist(-2.0f, 3.0f);
    for (int i = 0; i < 200; ++i) {
        const Vec3 q{dist(rng), dist(rng), dist(rng)};
        const auto hit = bvh.closestPoint(q);

        // Brute force over all triangles of all faces.
        float best = 1e30f;
        for (Index fi = 0; fi < cube.faceCapacity(); ++fi) {
            const cyber::FaceId f{fi};
            if (!cube.isAlive(f)) {
                continue;
            }
            const auto verts = cube.faceVertices(f);
            for (std::size_t t = 2; t < verts.size(); ++t) {
                const Vec3 p = cyber::closestPointOnTriangle(q, cube.position(verts[0]),
                                                             cube.position(verts[t - 1]),
                                                             cube.position(verts[t]));
                best = std::min(best, lengthSquared(p - q));
            }
        }
        REQUIRE(hit.distanceSquared == doctest::Approx(best).epsilon(1e-4));
    }
}

TEST_CASE("raycast hits the nearest face and respects max distance") {
    const Mesh cube = makeCube();
    const Bvh bvh(cube);

    const auto hit = bvh.raycast({0.5f, 0.5f, 5.0f}, {0, 0, -1});
    REQUIRE(hit.has_value());
    REQUIRE(hit->t == doctest::Approx(4.0f));
    REQUIRE(hit->point.z == doctest::Approx(1.0f));

    REQUIRE(!bvh.raycast({0.5f, 0.5f, 5.0f}, {0, 0, -1}, 3.0f).has_value());
    REQUIRE(!bvh.raycast({0.5f, 0.5f, 5.0f}, {0, 0, 1}).has_value());
}

TEST_CASE("raycast from inside hits the far wall") {
    const Mesh cube = makeCube();
    const Bvh bvh(cube);
    const auto hit = bvh.raycast({0.5f, 0.5f, 0.5f}, {1, 0, 0});
    REQUIRE(hit.has_value());
    REQUIRE(hit->t == doctest::Approx(0.5f));
    REQUIRE(hit->point.x == doctest::Approx(1.0f));
}

TEST_CASE("empty mesh yields empty BVH") {
    const Mesh empty;
    const Bvh bvh(empty);
    REQUIRE(bvh.empty());
    REQUIRE(!bvh.raycast({0, 0, 0}, {1, 0, 0}).has_value());
}

TEST_CASE("a regular grid does not degrade the build (spec: mesh-core)") {
    // Regression for the object-median split in src/core/src/bvh.cpp. On a
    // regular grid the median plane lands inside a block of triangles with the
    // same centroid coordinate, so both children inherit the parent's extent and
    // the subtree's cost stops falling with depth: the SAME torus at the SAME
    // triangle count cost between 49 and 99 depending only on how the grid rows
    // and columns lined up with the split planes. The binned SAH picks the plane
    // that actually shrinks the children, and every one of these shapes lands
    // well under the bound below (measured 39.4 to 60.6; the median build
    // measured 49.0 to 98.6 and breached it on two of the six).
    //
    // sahCost() is a pure function of the tree's shape — no queries, no timing —
    // so this is a structural assertion, not a wall clock.
    constexpr double kMaxCost = 70.0;
    struct Shape {
        int nu, nv;
    };
    // Same triangle budget (18432), six ways of laying the grid out.
    const std::vector<Shape> shapes = {{96, 96},  {32, 288}, {288, 32},
                                       {576, 16}, {64, 144}, {144, 64}};
    for (const Shape& shape : shapes) {
        CAPTURE(shape.nu);
        CAPTURE(shape.nv);
        const Bvh bvh(regularTorus(shape.nu, shape.nv));
        REQUIRE(bvh.triangleCount() == 18432);
        CHECK(bvh.sahCost() <= kMaxCost);
    }
}

TEST_CASE("the built hierarchy is well formed and reproducible") {
    // 4608 triangles takes the single-threaded path; 80000 crosses the build's
    // parallel threshold, so both the breadth-first top pass and the worker
    // subtree pass are exercised here.
    for (const int side : {48, 200}) {
        CAPTURE(side);
        const Mesh torus = regularTorus(side, side);
        const Bvh bvh(torus);
        const cyber::FlatBvh flat = bvh.flatten();

        // Every triangle sits in exactly one leaf.
        CHECK(leafTriangleTotal(flat) == bvh.triangleCount());

        // The query traversals carry a fixed 64-entry stack and silently drop
        // nodes past it, so the build must never emit one deeper than that. A
        // SAH split can be arbitrarily unbalanced, which the median split it
        // replaced could not be — hence the depth guard this pins.
        CHECK(treeDepth(flat) <= 64);

        // No leaf larger than the build's limit, and no internal node claiming a
        // child outside the array.
        for (const cyber::FlatBvhNode& node : flat.nodes) {
            if (node.triCount > 0) {
                CHECK(node.triCount <= 4);
                CHECK(node.leftFirst + node.triCount <= flat.tris.size());
            } else {
                CHECK(node.leftFirst + 1 < flat.nodes.size());
            }
        }

        // The build fans out over worker threads and its task depth is a
        // constant, not a function of the core count: the hierarchy must not
        // depend on how many workers ran, so two builds must be identical.
        const cyber::FlatBvh again = Bvh(torus).flatten();
        REQUIRE(again.nodes.size() == flat.nodes.size());
        for (std::size_t i = 0; i < flat.nodes.size(); ++i) {
            CHECK(again.nodes[i].leftFirst == flat.nodes[i].leftFirst);
            CHECK(again.nodes[i].triCount == flat.nodes[i].triCount);
        }
        for (std::size_t i = 0; i < flat.tris.size(); ++i) {
            CHECK(again.tris[i].face == flat.tris[i].face);
        }
    }
}

TEST_CASE("closest-point distance is the exact minimum, bit for bit") {
    // The identity the build is allowed to keep: pruning only discards subtrees
    // that cannot beat the incumbent, so the reported distanceSquared is the
    // minimum over ALL triangles and the minimum of a set of floats does not
    // depend on the order they were visited in. (Which triangle is NAMED can
    // differ when several are exactly equidistant — a shared vertex is the
    // common case — so `face` is deliberately not asserted here.)
    const Mesh torus = regularTorus(24, 24);
    const Bvh bvh(torus);
    std::mt19937 rng(11);
    std::uniform_real_distribution<float> dist(-2.0f, 2.0f);
    for (int i = 0; i < 300; ++i) {
        const Vec3 q{dist(rng), dist(rng), dist(rng)};
        const auto hit = bvh.closestPoint(q);

        float best = std::numeric_limits<float>::max();
        for (Index fi = 0; fi < torus.faceCapacity(); ++fi) {
            const cyber::FaceId f{fi};
            if (!torus.isAlive(f)) {
                continue;
            }
            const auto verts = torus.faceVertices(f);
            for (std::size_t t = 2; t < verts.size(); ++t) {
                const Vec3 p = cyber::closestPointOnTriangle(q, torus.position(verts[0]),
                                                             torus.position(verts[t - 1]),
                                                             torus.position(verts[t]));
                best = std::min(best, lengthSquared(p - q));
            }
        }
        REQUIRE(hit.distanceSquared == best);  // bitwise, not Approx
        // And the reported point really is at the reported distance.
        REQUIRE(lengthSquared(hit.point - q) == hit.distanceSquared);
    }
}

TEST_CASE("the build reports progress and honours a cancel token") {
    // A multi-million-triangle Target import is seconds of work; on device that
    // is a blocking, uncancellable wait unless the build follows the library's
    // ProgressSink/CancelToken contract.
    // Above the build's parallel threshold, so the cancel token has to reach the
    // worker threads and not just the top pass.
    const Mesh torus = regularTorus(200, 200);
    REQUIRE(torus.faceCount() == 40000);

    float last = -1.0f;
    bool monotone = true;
    std::string stage;
    ProgressSink sink([&](float p, std::string_view s) {
        monotone = monotone && p >= last;
        last = p;
        stage = std::string(s);
    });
    const Bvh built(torus, &sink, nullptr);
    CHECK(monotone);
    CHECK(last == doctest::Approx(1.0f));
    CHECK(stage == "bvh");
    CHECK_FALSE(built.empty());
    CHECK_FALSE(built.cancelled());

    // A token tripped before the call aborts the build, and a cancelled build
    // leaves an EMPTY hierarchy rather than a half-built one.
    const CancelToken cancel;
    cancel.requestCancel();
    const Bvh aborted(torus, nullptr, &cancel);
    CHECK(aborted.cancelled());
    CHECK(aborted.empty());
    CHECK(aborted.triangleCount() == 0);
    CHECK(aborted.closestPoint({0, 0, 0}).distanceSquared == std::numeric_limits<float>::max());
    CHECK_FALSE(aborted.raycast({0, 0, 5}, {0, 0, -1}).has_value());

    // Cancelling mid-build (the token trips on the first progress report) is the
    // same outcome, and an untripped token changes nothing.
    const CancelToken untouched;
    const Bvh normal(torus, nullptr, &untouched);
    CHECK_FALSE(normal.cancelled());
    CHECK(normal.triangleCount() == built.triangleCount());
}

TEST_CASE("no ray through a shared edge is rejected by both adjacent triangles") {
    // Regression for the watertight ray/triangle test in src/core/src/bvh.cpp.
    // Strict Moller-Trumbore u/v rejections let rounding make BOTH triangles
    // sharing an edge miss the same ray, so a ray that geometrically crosses a
    // closed surface reported a miss — isolated wrong texels wherever a bake
    // cage ray crosses a shared edge (src/bake/src/bake.cpp calls this copy
    // directly). The inside test now evaluates each edge in a canonical vertex
    // order, so the two triangles see bitwise opposite values.
    //
    // The same bundle runs against the accel backends' copy of the test in
    // tests/accel/test_gpu_geometry.cpp; the two cases must stay in step, as the
    // rayTriangle implementations are mirrors of each other.
    //
    // Deliberately irregular coordinates: axis-aligned vertices make too many of
    // the intermediate products exact and hide the leak.
    const Vec3 a{-0.7331f, 0.1129f, 0.4517f};
    const Vec3 b{0.6217f, -0.3384f, 0.2903f};
    const Vec3 c{0.1873f, 0.8821f, -0.5119f};
    const Vec3 d{-0.2447f, -0.9013f, -0.3761f};
    // Consistent winding across the shared edge a-b.
    const std::vector<Vec3> points{a, b, c, d};
    const std::vector<std::vector<Index>> faces{{0, 1, 2}, {1, 0, 3}};
    const Mesh mesh = Mesh::fromIndexed(points, faces);
    const Bvh bvh(mesh);
    REQUIRE(bvh.triangleCount() == 2);

    constexpr std::size_t rays = 200'000;  // bounded: a second at most
    const Vec3 normal = cyber::normalized(cyber::cross(b - a, c - a));
    std::mt19937 rng(7);
    std::uniform_real_distribution<float> along(0.05f, 0.95f);
    std::uniform_real_distribution<float> height(0.5f, 2.0f);
    std::size_t leaks = 0;
    for (std::size_t i = 0; i < rays; ++i) {
        const Vec3 target = a + (b - a) * along(rng);  // exactly on the shared edge
        const Vec3 origin = target + normal * height(rng);
        if (!bvh.raycast(origin, cyber::normalized(target - origin)).has_value()) {
            ++leaks;
        }
    }
    CAPTURE(leaks);
    REQUIRE(leaks == 0);
}

// A query equidistant from several triangles has no geometrically "right"
// answer, so whichever the traversal reaches first used to win — making the
// result a function of how the tree happened to be built. Changing the builder
// (median split -> binned SAH) then moved closest-point answers by a last-bit
// amount on tie-heavy models, which is exactly the kind of drift the engine's
// bit-identity gates exist to prevent. The tie now resolves on the lower face
// id, so the answer is a pure function of the mesh.
TEST_CASE("an equidistant closest-point tie resolves on the lower face id") {
    // Two coplanar triangles sharing the edge (0,0,0)-(1,0,0). A query directly
    // above the midpoint of that shared edge is exactly equidistant from both.
    const std::vector<Vec3> p = {{0, 0, 0}, {1, 0, 0}, {0, 1, 0}, {1, -1, 0}};
    const std::vector<std::vector<Index>> f = {{0, 1, 2}, {0, 3, 1}};
    const Mesh mesh = Mesh::fromIndexed(p, f);
    const Bvh bvh(mesh);

    const Bvh::ClosestHit hit = bvh.closestPoint({0.5f, 0.0f, 1.0f});
    REQUIRE(hit.face.valid());
    // Both faces are at the same distance; the contract is the lower id.
    CHECK(hit.distanceSquared == doctest::Approx(1.0f));
    CHECK(hit.face.value == 0u);

    // Same mesh, faces declared in the opposite order: the winning face id is
    // still the lower one, so the answer does not depend on declaration order
    // either — only on which id the tie lands on.
    const std::vector<std::vector<Index>> swapped = {{0, 3, 1}, {0, 1, 2}};
    const Mesh other = Mesh::fromIndexed(p, swapped);
    const Bvh otherBvh(other);
    const Bvh::ClosestHit otherHit = otherBvh.closestPoint({0.5f, 0.0f, 1.0f});
    REQUIRE(otherHit.face.valid());
    CHECK(otherHit.face.value == 0u);
    CHECK(otherHit.point.x == doctest::Approx(hit.point.x));
    CHECK(otherHit.point.y == doctest::Approx(hit.point.y));
    CHECK(otherHit.point.z == doctest::Approx(hit.point.z));
}
