#include <doctest.h>

#include <algorithm>
#include <cmath>
#include <cstdint>
#include <random>
#include <vector>

#include "cyber/accel/backend.hpp"
#include "cyber/accel/detail/bvh_residency.hpp"
#include "cyber/core/bvh.hpp"
#include "cyber/core/math.hpp"
#include "cyber/core/mesh.hpp"

// GPU geometry parity harness (roadmap 4.6/5.8/11.1): closest-point and raycast
// now run on-device via IBackend::closestPointsBvh / raycastBvh over a flattened
// BVH. Every available backend must match the CPU reference (which itself
// matches the core Bvh) within f32 tolerance. On cpu-headless this reduces to a
// self-check of the reference + a validation of Bvh::flatten().
namespace accel = cyber::accel;
using cyber::Bvh;
using cyber::FlatBvh;
using cyber::Index;
using cyber::Mesh;
using cyber::Vec3;

namespace {

// Coordinates on a unit sphere stay near [-1, 1], so an absolute tolerance is
// appropriate; GPU float reassociation and the JIT'd kernels stay well inside.
constexpr float kTol = 3e-3f;

Mesh makeSphere(int stacks, int slices) {
    std::vector<Vec3> points;
    std::vector<std::vector<Index>> faces;
    // North pole, rings, south pole.
    points.push_back({0.0f, 1.0f, 0.0f});
    for (int i = 1; i < stacks; ++i) {
        const float phi = cyber::kPi * static_cast<float>(i) / static_cast<float>(stacks);
        const float y = std::cos(phi);
        const float r = std::sin(phi);
        for (int j = 0; j < slices; ++j) {
            const float theta =
                2.0f * cyber::kPi * static_cast<float>(j) / static_cast<float>(slices);
            points.push_back({r * std::cos(theta), y, r * std::sin(theta)});
        }
    }
    const auto south = static_cast<Index>(points.size());
    points.push_back({0.0f, -1.0f, 0.0f});

    auto ring = [slices](int stack, int j) {
        return static_cast<Index>(1 + (stack - 1) * slices + (j % slices));
    };
    // Top cap (triangles).
    for (int j = 0; j < slices; ++j) {
        faces.push_back({0, ring(1, j + 1), ring(1, j)});
    }
    // Middle quads.
    for (int i = 1; i < stacks - 1; ++i) {
        for (int j = 0; j < slices; ++j) {
            faces.push_back({ring(i, j), ring(i, j + 1), ring(i + 1, j + 1), ring(i + 1, j)});
        }
    }
    // Bottom cap (triangles).
    for (int j = 0; j < slices; ++j) {
        faces.push_back({south, ring(stacks - 1, j), ring(stacks - 1, j + 1)});
    }
    return Mesh::fromIndexed(points, faces);
}

}  // namespace

TEST_CASE("flattened-BVH closest point and raycast match the CPU reference on every backend") {
    const Mesh sphere = makeSphere(16, 24);
    const Bvh bvh(sphere);
    REQUIRE(!bvh.empty());
    const FlatBvh flat = bvh.flatten();
    REQUIRE(!flat.nodes.empty());
    REQUIRE(flat.tris.size() == bvh.triangleCount());

    std::mt19937 rng(2024);
    std::uniform_real_distribution<float> box(-1.6f, 1.6f);
    std::uniform_real_distribution<float> ball(-0.3f, 0.3f);
    std::uniform_real_distribution<float> shell(2.0f, 3.0f);
    std::uniform_real_distribution<float> unit(-1.0f, 1.0f);

    constexpr std::size_t n = 1024;

    // Closest-point queries: random points in a box around the sphere.
    std::vector<float> queries(n * 3);
    for (std::size_t i = 0; i < n; ++i) {
        queries[i * 3] = box(rng);
        queries[i * 3 + 1] = box(rng);
        queries[i * 3 + 2] = box(rng);
    }

    // Rays: half aimed through a small central ball (guaranteed hits), half
    // aimed away from the sphere (guaranteed misses) to exercise both paths.
    std::vector<float> origins(n * 3);
    std::vector<float> dirs(n * 3);
    for (std::size_t i = 0; i < n; ++i) {
        Vec3 o{unit(rng), unit(rng), unit(rng)};
        const float s = shell(rng) / std::sqrt(std::max(cyber::lengthSquared(o), 1e-6f));
        o = o * s;  // on a shell of radius ~2..3
        const Vec3 target{ball(rng), ball(rng), ball(rng)};
        Vec3 d = (i % 2 == 0) ? (target - o) : (o - target);  // toward vs away
        d = cyber::normalized(d);
        origins[i * 3] = o.x;
        origins[i * 3 + 1] = o.y;
        origins[i * 3 + 2] = o.z;
        dirs[i * 3] = d.x;
        dirs[i * 3 + 1] = d.y;
        dirs[i * 3 + 2] = d.z;
    }

    const auto backends = accel::availableBackends();
    REQUIRE(!backends.empty());
    const auto& cpu = backends.back();
    REQUIRE(cpu->kind() == accel::BackendKind::Cpu);

    // CPU reference results.
    std::vector<float> refClosest(n * 3);
    cpu->closestPointsBvh(flat.nodes.data(), flat.nodes.size(), flat.tris.data(), flat.tris.size(),
                          queries.data(), n, refClosest.data());
    std::vector<float> refHit(n * 3);
    std::vector<int> refFace(n);
    cpu->raycastBvh(flat.nodes.data(), flat.nodes.size(), flat.tris.data(), flat.tris.size(),
                    origins.data(), dirs.data(), n, refHit.data(), refFace.data());

    // The CPU reference must reproduce the core Bvh per-query traversal exactly
    // (validates Bvh::flatten() and the flat-array traversal).
    for (std::size_t i = 0; i < n; ++i) {
        const Vec3 q{queries[i * 3], queries[i * 3 + 1], queries[i * 3 + 2]};
        const Vec3 core = bvh.closestPoint(q).point;
        REQUIRE(refClosest[i * 3] == doctest::Approx(core.x).epsilon(kTol));
        REQUIRE(refClosest[i * 3 + 1] == doctest::Approx(core.y).epsilon(kTol));
        REQUIRE(refClosest[i * 3 + 2] == doctest::Approx(core.z).epsilon(kTol));
    }

    for (const auto& backend : backends) {
        CAPTURE(backend->deviceName());

        std::vector<float> closest(n * 3);
        backend->closestPointsBvh(flat.nodes.data(), flat.nodes.size(), flat.tris.data(),
                                  flat.tris.size(), queries.data(), n, closest.data());
        for (std::size_t i = 0; i < n * 3; ++i) {
            REQUIRE(closest[i] == doctest::Approx(refClosest[i]).epsilon(kTol));
        }

        std::vector<float> hit(n * 3);
        std::vector<int> face(n);
        backend->raycastBvh(flat.nodes.data(), flat.nodes.size(), flat.tris.data(),
                            flat.tris.size(), origins.data(), dirs.data(), n, hit.data(),
                            face.data());
        for (std::size_t i = 0; i < n; ++i) {
            CAPTURE(i);
            const bool refMiss = refFace[i] < 0;
            const bool gpuMiss = face[i] < 0;
            REQUIRE(refMiss == gpuMiss);
            if (refMiss) {
                continue;
            }
            const Vec3 rp{refHit[i * 3], refHit[i * 3 + 1], refHit[i * 3 + 2]};
            const Vec3 gp{hit[i * 3], hit[i * 3 + 1], hit[i * 3 + 2]};
            REQUIRE(cyber::length(rp - gp) < kTol);
            // Faces agree except at a shared-edge tie, where both hit points
            // coincide and either owning face is a valid answer.
            if (face[i] != refFace[i]) {
                REQUIRE(cyber::length(rp - gp) < kTol);
            }
        }
    }
}

TEST_CASE("no ray through a shared edge is rejected by both adjacent triangles") {
    // Regression: the ray/triangle test rejected u/v strictly at 0 and 1 with no
    // edge-ownership rule, so rounding could make BOTH triangles sharing an edge
    // reject the same ray and a ray that geometrically crosses a closed surface
    // report a miss — isolated wrong texels in an AO bake. The inside test now
    // evaluates each edge in a canonical vertex order, so the two triangles see
    // bitwise opposite values and at most one of them can reject.
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
    Mesh mesh = Mesh::fromIndexed(points, faces);
    const Bvh bvh(mesh);
    REQUIRE(bvh.triangleCount() == 2);
    const FlatBvh flat = bvh.flatten();

    constexpr std::size_t rays = 200'000;  // bounded: seconds at most, never unbounded
    std::vector<float> origins(rays * 3);
    std::vector<float> dirs(rays * 3);
    const Vec3 normal = cyber::normalized(cyber::cross(b - a, c - a));
    std::mt19937 rng(7);
    std::uniform_real_distribution<float> along(0.05f, 0.95f);
    std::uniform_real_distribution<float> height(0.5f, 2.0f);
    for (std::size_t i = 0; i < rays; ++i) {
        const Vec3 target = a + (b - a) * along(rng);  // exactly on the shared edge
        const Vec3 origin = target + normal * height(rng);
        const Vec3 dir = cyber::normalized(target - origin);
        origins[i * 3] = origin.x;
        origins[i * 3 + 1] = origin.y;
        origins[i * 3 + 2] = origin.z;
        dirs[i * 3] = dir.x;
        dirs[i * 3 + 1] = dir.y;
        dirs[i * 3 + 2] = dir.z;
    }

    const auto backend = accel::defaultBackend();
    std::vector<float> hit(rays * 3);
    std::vector<int> face(rays);
    backend->raycastBvh(flat.nodes.data(), flat.nodes.size(), flat.tris.data(), flat.tris.size(),
                        origins.data(), dirs.data(), rays, hit.data(), face.data());
    std::size_t leaks = 0;
    for (std::size_t i = 0; i < rays; ++i) {
        if (face[i] < 0) {
            ++leaks;
        }
    }
    CAPTURE(leaks);
    REQUIRE(leaks == 0);
}

TEST_CASE("BVH residency key distinguishes a recycled buffer from a re-query") {
    // GPU backends keep the last-queried BVH resident instead of re-uploading it
    // per call (the AO bake queries once per texel). Keying on addresses and
    // sizes alone would serve a stale hierarchy when a freed FlatBvh's storage
    // is reused at the same address with the same counts, so the key also
    // carries a constant-time content fingerprint.
    namespace detail = cyber::accel::detail;
    Mesh mesh = makeSphere(6, 8);
    const Bvh bvh(mesh);
    FlatBvh flat = bvh.flatten();
    REQUIRE(!flat.tris.empty());

    const auto key = detail::makeBvhResidencyKey(flat.nodes.data(), flat.nodes.size(),
                                                 flat.tris.data(), flat.tris.size());
    REQUIRE(key == detail::makeBvhResidencyKey(flat.nodes.data(), flat.nodes.size(),
                                               flat.tris.data(), flat.tris.size()));

    // Same addresses and counts, different contents: must miss.
    flat.tris.front().a[0] += 1.0f;
    REQUIRE(key != detail::makeBvhResidencyKey(flat.nodes.data(), flat.nodes.size(),
                                               flat.tris.data(), flat.tris.size()));
    flat.tris.front().a[0] -= 1.0f;
    REQUIRE(key == detail::makeBvhResidencyKey(flat.nodes.data(), flat.nodes.size(),
                                               flat.tris.data(), flat.tris.size()));
    flat.tris.back().c[2] += 1.0f;
    REQUIRE(key != detail::makeBvhResidencyKey(flat.nodes.data(), flat.nodes.size(),
                                               flat.tris.data(), flat.tris.size()));
    flat.tris.back().c[2] -= 1.0f;

    // A shorter view of the same arrays is a different BVH.
    REQUIRE(key != detail::makeBvhResidencyKey(flat.nodes.data(), flat.nodes.size(),
                                               flat.tris.data(), flat.tris.size() - 1));
    // An empty BVH keys consistently rather than reading through null.
    const auto empty = detail::makeBvhResidencyKey(nullptr, 0, nullptr, 0);
    REQUIRE(empty == detail::makeBvhResidencyKey(nullptr, 0, nullptr, 0));
    REQUIRE(empty != key);
}
