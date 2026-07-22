#include <doctest.h>

#include <algorithm>
#include <cstddef>
#include <numeric>
#include <optional>
#include <vector>

#include "cyber/accel/backend.hpp"
#include "cyber/accel/buffer.hpp"
#include "cyber/accel/primitives.hpp"
#include "cyber/core/bvh.hpp"
#include "cyber/core/mesh.hpp"

using cyber::Bvh;
using cyber::Index;
using cyber::Mesh;
using cyber::Vec3;
namespace accel = cyber::accel;

namespace {

Mesh makeQuadPlane() {
    const std::vector<Vec3> p = {{0, 0, 0}, {1, 0, 0}, {1, 1, 0}, {0, 1, 0}};
    const std::vector<std::vector<Index>> f = {{0, 1, 2}, {0, 2, 3}};
    return Mesh::fromIndexed(p, f);
}

}  // namespace

TEST_CASE("buffer round-trips host data through upload/download") {
    accel::Buffer<int> buf;
    buf.upload({1, 2, 3, 4});
    REQUIRE(buf.size() == 4);
    std::vector<int> out;
    buf.download(out);
    REQUIRE(out == std::vector<int>{1, 2, 3, 4});
}

TEST_CASE("map applies the kernel across the whole buffer") {
    auto backend = accel::defaultBackend();
    accel::Buffer<int> in{1, 2, 3, 4, 5};
    accel::Buffer<int> out;
    accel::map(*backend, in, out, [](int v) { return v * v; });
    REQUIRE(out.host() == std::vector<int>{1, 4, 9, 16, 25});
}

TEST_CASE("reduce matches a serial fold regardless of chunking") {
    auto backend = accel::defaultBackend();
    std::vector<long> values(100'000);
    std::iota(values.begin(), values.end(), 1);
    accel::Buffer<long> in(values);
    const long sum = accel::reduce(*backend, in, 0L, [](long a, long b) { return a + b; });
    REQUIRE(sum == 100'000L * 100'001L / 2);
}

TEST_CASE("reduce over an empty buffer returns the seed") {
    auto backend = accel::defaultBackend();
    accel::Buffer<int> in;
    REQUIRE(accel::reduce(*backend, in, 42, [](int a, int b) { return a + b; }) == 42);
}

TEST_CASE("inclusive and exclusive scans agree with hand computation") {
    auto backend = accel::defaultBackend();
    accel::Buffer<int> in{3, 1, 4, 1, 5};
    auto add = [](int a, int b) { return a + b; };

    accel::Buffer<int> inclusive;
    accel::inclusiveScan(*backend, in, inclusive, add);
    REQUIRE(inclusive.host() == std::vector<int>{3, 4, 8, 9, 14});

    accel::Buffer<int> exclusive;
    accel::exclusiveScan(*backend, in, exclusive, 0, add);
    REQUIRE(exclusive.host() == std::vector<int>{0, 3, 4, 8, 9});
}

TEST_CASE("sort orders a buffer in place") {
    auto backend = accel::defaultBackend();
    accel::Buffer<int> buf{5, 3, 8, 1, 9, 2};
    accel::sort(*backend, buf, [](int a, int b) { return a < b; });
    REQUIRE(buf.host() == std::vector<int>{1, 2, 3, 5, 8, 9});
}

TEST_CASE("spmv computes y = A * x for a known CSR matrix") {
    // A = [[2, 0, 1],
    //      [0, 3, 0],
    //      [4, 0, 5]]
    accel::SparseMatrix a;
    a.rows = 3;
    a.rowStart = {0, 2, 3, 5};
    a.colIndex = {0, 2, 1, 0, 2};
    a.value = {2, 1, 3, 4, 5};
    accel::Buffer<float> x{1, 2, 3};
    accel::Buffer<float> y;
    auto backend = accel::defaultBackend();
    accel::spmv(*backend, a, x, y);
    // [2*1 + 1*3, 3*2, 4*1 + 5*3] = [5, 6, 19]
    REQUIRE(y.host() == std::vector<float>{5, 6, 19});
}

TEST_CASE("batched closest-point matches per-query BVH traversal") {
    Mesh mesh = makeQuadPlane();
    const Bvh bvh(mesh);
    accel::Buffer<Vec3> queries{{0.3f, 0.3f, 1.0f}, {0.8f, 0.1f, -2.0f}, {2.0f, 2.0f, 0.0f}};
    accel::Buffer<Vec3> out;
    auto backend = accel::defaultBackend();
    accel::closestPoints(*backend, bvh, queries, out);

    REQUIRE(out.size() == queries.size());
    for (std::size_t i = 0; i < queries.size(); ++i) {
        const Vec3 expected = bvh.closestPoint(queries[i]).point;
        REQUIRE(out[i].x == doctest::Approx(expected.x));
        REQUIRE(out[i].y == doctest::Approx(expected.y));
        REQUIRE(out[i].z == doctest::Approx(expected.z));
    }
}

TEST_CASE("batched raycast matches per-ray BVH traversal") {
    Mesh mesh = makeQuadPlane();
    const Bvh bvh(mesh);
    accel::Buffer<Vec3> origins{{0.3f, 0.3f, 1.0f}, {5.0f, 5.0f, 1.0f}};
    accel::Buffer<Vec3> directions{{0, 0, -1}, {0, 0, -1}};
    accel::Buffer<std::optional<Bvh::RayHit>> out;
    auto backend = accel::defaultBackend();
    accel::raycast(*backend, bvh, origins, directions, out);

    REQUIRE(out.size() == 2);
    REQUIRE(out[0].has_value());  // straight down onto the plane
    REQUIRE(out[0]->point.z == doctest::Approx(0.0f));
    REQUIRE_FALSE(out[1].has_value());  // misses the unit quad
}
