#include <doctest.h>

#include <algorithm>
#include <chrono>
#include <cmath>
#include <cstddef>
#include <functional>
#include <memory>
#include <mutex>
#include <set>
#include <string>
#include <utility>
#include <vector>

#include "cyber/accel/backend.hpp"
#include "cyber/bake/bake.hpp"
#include "cyber/core/mesh.hpp"
#include "cyber/core/progress.hpp"

using cyber::CancelToken;
using cyber::FaceId;
using cyber::Index;
using cyber::LoopId;
using cyber::Mesh;
using cyber::Vec2;
using cyber::Vec3;
namespace accel = cyber::accel;
namespace bake = cyber::bake;

namespace {

// Flat low-poly quad in the z=0 plane over [0,1]^2 with uv == xy.
Mesh lowPlane() {
    const std::vector<Vec3> p = {{0, 0, 0}, {1, 0, 0}, {1, 1, 0}, {0, 1, 0}};
    const std::vector<std::vector<Index>> f = {{0, 1, 2, 3}};
    Mesh mesh = Mesh::fromIndexed(p, f);
    auto& uv = mesh.cornerAttributes().create<Vec2>("uv");
    for (Index fi = 0; fi < mesh.faceCapacity(); ++fi) {
        if (!mesh.isAlive(FaceId{fi})) {
            continue;
        }
        for (const LoopId l : mesh.faceLoops(FaceId{fi})) {
            const Vec3 pos = mesh.position(mesh.loopVertex(l));
            uv[l.value] = {pos.x, pos.y};
        }
    }
    return mesh;
}

// High-poly: a flat base coplanar with the low-poly, plus a short vertical ridge
// wall standing at x=0.5 (its own vertices, so the base keeps a clean +z
// normal). A smooth low-poly baked against this should darken beside the ridge.
Mesh highWithRidge() {
    const std::vector<Vec3> p = {// base quad [0,1]^2 at z=0
                                 {0, 0, 0},
                                 {1, 0, 0},
                                 {1, 1, 0},
                                 {0, 1, 0},
                                 // ridge wall at x=0.5, y in [0,1], z in [0,0.15]
                                 {0.5f, 0, 0},
                                 {0.5f, 1, 0},
                                 {0.5f, 1, 0.15f},
                                 {0.5f, 0, 0.15f}};
    const std::vector<std::vector<Index>> f = {{0, 1, 2, 3}, {4, 5, 6, 7}};
    return Mesh::fromIndexed(p, f);
}

// High-poly: the flat base plus a wide ceiling tilted in x, clearing the cage
// (z >= 0.25 > 2*cageDistance) so the projection ray still lands on the base.
// Openness under it therefore sweeps smoothly from dark at x=0 to open at x=1,
// which is the gradient banding is visible on.
Mesh tiltedCeiling() {
    // Ceiling plane z = 0.25 + 0.6x: openness under it runs ~0.06 at x=0 to
    // ~0.7 at x=1 with aoRadius 1, so the gradient covers the whole layout.
    const std::vector<Vec3> p = {{0, 0, 0},        {1, 0, 0},          {1, 1, 0},
                                 {0, 1, 0},        {-0.2f, -1, 0.13f}, {1.2f, -1, 0.97f},
                                 {1.2f, 2, 0.97f}, {-0.2f, 2, 0.13f}};
    const std::vector<std::vector<Index>> f = {{0, 1, 2, 3}, {4, 5, 6, 7}};
    return Mesh::fromIndexed(p, f);
}

// Fraction of horizontally adjacent covered-texel pairs whose 8-bit encodings
// differ. Flat bands score near 0 (only the band edges transition); dithered
// noise scores high. Uncovered texels are exactly 0 and are skipped.
float adjacentVariation(const bake::Image& img) {
    int pairs = 0;
    int differ = 0;
    const auto quantize = [](float v) {
        return static_cast<int>(std::lround(std::clamp(v, 0.0f, 1.0f) * 255.0f));
    };
    for (int y = 0; y < img.height; ++y) {
        for (int x = 0; x + 1 < img.width; ++x) {
            const float a = img.at(x, y, 0);
            const float b = img.at(x + 1, y, 0);
            if (a <= 0.0f || b <= 0.0f) {
                continue;
            }
            ++pairs;
            differ += quantize(a) != quantize(b) ? 1 : 0;
        }
    }
    return pairs == 0 ? 0.0f : static_cast<float>(differ) / static_cast<float>(pairs);
}

// Distinct 8-bit levels among covered texels.
std::size_t distinctLevels(const bake::Image& img) {
    std::set<int> levels;
    for (int y = 0; y < img.height; ++y) {
        for (int x = 0; x < img.width; ++x) {
            const float v = img.at(x, y, 0);
            if (v > 0.0f) {
                levels.insert(static_cast<int>(std::lround(std::clamp(v, 0.0f, 1.0f) * 255.0f)));
            }
        }
    }
    return levels.size();
}

// AO openness sampled at layout UV (u, v); uv == xy for lowPlane().
float aoAt(const bake::Image& img, float u, float v) {
    int px = static_cast<int>(u * static_cast<float>(img.width));
    int py = static_cast<int>((1.0f - v) * static_cast<float>(img.height));
    px = std::max(0, std::min(img.width - 1, px));
    py = std::max(0, std::min(img.height - 1, py));
    return img.at(px, py, 0);
}

// Flat target over [0,1]^2 at z=0, subdivided into n x n quads: the same
// geometry lowPlane() bakes against, at an arbitrary triangle count.
Mesh densePlane(int n) {
    std::vector<Vec3> p;
    const float step = 1.0f / static_cast<float>(n);
    for (int y = 0; y <= n; ++y) {
        for (int x = 0; x <= n; ++x) {
            p.push_back({static_cast<float>(x) * step, static_cast<float>(y) * step, 0.0f});
        }
    }
    const auto vertexAt = [n](int x, int y) { return static_cast<Index>(y * (n + 1) + x); };
    std::vector<std::vector<Index>> f;
    for (int y = 0; y < n; ++y) {
        for (int x = 0; x < n; ++x) {
            f.push_back(
                {vertexAt(x, y), vertexAt(x + 1, y), vertexAt(x + 1, y + 1), vertexAt(x, y + 1)});
        }
    }
    return Mesh::fromIndexed(p, f);
}

// Backend that records the ranges a bake hands to parallelFor and delegates the
// work to the CPU reference, so a test can see WHERE the parallelism sits.
class RecordingBackend final : public accel::IBackend {
public:
    explicit RecordingBackend(std::shared_ptr<accel::IBackend> inner) : m_inner(std::move(inner)) {}

    [[nodiscard]] accel::BackendKind kind() const override { return m_inner->kind(); }
    [[nodiscard]] std::string deviceName() const override { return m_inner->deviceName(); }

    void parallelFor(std::size_t begin, std::size_t end,
                     const std::function<void(std::size_t, std::size_t)>& fn) override {
        {
            const std::lock_guard<std::mutex> lock(m_mutex);
            m_ranges.push_back(end - begin);
        }
        m_inner->parallelFor(begin, end, fn);
    }

    [[nodiscard]] std::vector<std::size_t> ranges() const {
        const std::lock_guard<std::mutex> lock(m_mutex);
        return m_ranges;
    }

private:
    std::shared_ptr<accel::IBackend> m_inner;
    mutable std::mutex m_mutex;
    std::vector<std::size_t> m_ranges;
};

// Installs a process-wide backend for the duration of a test.
class ScopedBackend {
public:
    explicit ScopedBackend(std::shared_ptr<accel::IBackend> backend)
        : m_previous(accel::defaultBackend()) {
        accel::setDefaultBackend(std::move(backend));
    }
    ~ScopedBackend() { accel::setDefaultBackend(m_previous); }
    ScopedBackend(const ScopedBackend&) = delete;
    ScopedBackend& operator=(const ScopedBackend&) = delete;

private:
    std::shared_ptr<accel::IBackend> m_previous;
};

// Wall-clock milliseconds of one AO bake.
long long aoBakeMs(const Mesh& low, const Mesh& high, const bake::BakeParams& p) {
    const auto start = std::chrono::steady_clock::now();
    const bake::BakeResult r = bake::bake(low, high, bake::BakeMap::AmbientOcclusion, p);
    const auto elapsed = std::chrono::steady_clock::now() - start;
    REQUIRE(r.texelsCovered > 0);
    return std::chrono::duration_cast<std::chrono::milliseconds>(elapsed).count();
}

bake::BakeParams aoParams() {
    bake::BakeParams p;
    p.width = 64;
    p.height = 64;
    p.cageDistance = 0.1f;
    p.aoSamples = 64;
    p.aoRadius = 1.0f;
    return p;
}

}  // namespace

TEST_CASE("AO projects the low-poly texel onto the high-poly before sampling") {
    const Mesh low = lowPlane();
    const Mesh high = highWithRidge();
    const bake::BakeResult r = bake::bake(low, high, bake::BakeMap::AmbientOcclusion, aoParams());
    REQUIRE_FALSE(r.image.pixels.empty());

    // Beside the ridge (u ~ 0.51) the projected high-poly hit is occluded by the
    // wall; far from it (u ~ 0.1) the hemisphere is open. The whole low-poly is a
    // smooth plane, so any darkening comes from projecting to the high-poly.
    const float occluded = aoAt(r.image, 0.51f, 0.5f);
    const float open = aoAt(r.image, 0.1f, 0.5f);

    CHECK(open > 0.8f);             // open away from the ridge
    CHECK(occluded < open - 0.2f);  // measurably darker beside the ridge
}

TEST_CASE("AO dithers instead of banding, and the default budget spans 8-bit") {
    // Regression: every texel fired the SAME Hammersley set, so openness could
    // only take k/aoSamples and neighbouring texels snapped to the same rung.
    // A smooth occlusion gradient came out as a handful of flat bands (a real
    // 16-sample bake showed 74% of charted texels pinned at 255 and NOTHING in
    // [240,254)). The per-texel rotation turns that into fine dither.
    const Mesh low = lowPlane();
    const Mesh high = tiltedCeiling();

    bake::BakeParams p;
    p.width = 64;
    p.height = 64;
    p.cageDistance = 0.1f;
    p.aoRadius = 1.0f;

    // Part 1 — banding, measured at a FIXED small budget so this isolates the
    // sample rotation from the default-count bump.
    p.aoSamples = 16;
    const bake::BakeResult banded = bake::bake(low, high, bake::BakeMap::AmbientOcclusion, p);
    REQUIRE_FALSE(banded.image.pixels.empty());
    const float varying = adjacentVariation(banded.image);
    // Piecewise-constant bands only transition at a band edge (measured 0.14
    // before the fix); dither differs on most neighbours (measured 0.67).
    CHECK(varying > 0.4f);

    // Part 2 — the default budget has to reach 8-bit resolution: 16 rays give
    // 17 possible rungs no matter how well they are dithered.
    const bake::BakeParams defaults{};
    CHECK(defaults.aoSamples >= 64);
    p.aoSamples = defaults.aoSamples;
    const bake::BakeResult fine = bake::bake(low, high, bake::BakeMap::AmbientOcclusion, p);
    // The gradient spans ~0.65 of the openness range, so 16 rays can only reach
    // ~11 levels here and 64 reach ~31.
    CHECK(distinctLevels(fine.image) > 25);

    // Part 3 — still deterministic: the rotation is a hash of the texel, not an
    // RNG, so a re-bake is bit-identical.
    const bake::BakeResult again = bake::bake(low, high, bake::BakeMap::AmbientOcclusion, p);
    CHECK(fine.image.pixels == again.image.pixels);
}

TEST_CASE("AO bake threads over the texel loop, not once per texel") {
    // Regression: the bake fired accel::raycast per texel and every call fanned
    // out ~hardware_concurrency threads for its handful of rays. The fan-out
    // dwarfed the rays it served (measured 3.0 s vs 61 ms for a 128x128 AO bake
    // of spot.obj), so the parallelism has to live on the texel loop and the
    // per-texel ray batch has to stay serial.
    auto recorder = std::make_shared<RecordingBackend>(accel::defaultBackend());
    const ScopedBackend installed(recorder);

    const Mesh low = lowPlane();
    const Mesh high = highWithRidge();
    const bake::BakeResult r = bake::bake(low, high, bake::BakeMap::AmbientOcclusion, aoParams());
    REQUIRE(r.texelsCovered > 1000);

    const std::vector<std::size_t> ranges = recorder->ranges();
    REQUIRE(!ranges.empty());
    const std::size_t spanningTexels =
        static_cast<std::size_t>(std::count(ranges.begin(), ranges.end(), r.texelsCovered));
    CHECK(spanningTexels == 1);
    // Anything else parallelFor sees is the per-texel ray batch, which is far
    // shorter than the texel loop.
    CHECK(*std::max_element(ranges.begin(), ranges.end()) == r.texelsCovered);
}

TEST_CASE("AO bake cost does not scale with Target triangle count") {
    // Regression: accel::raycast flattened the whole BVH on every call, so each
    // texel re-copied every node and triangle of the Target and bake time grew
    // linearly with Target density — the one axis a bake should be flat in.
    // Same layout, same rays, two Targets ~4 orders of magnitude apart in
    // triangle count: 3 ms vs 15 ms with the BVH flattened once, 4 ms vs 460 ms
    // with the per-call flatten restored. The bound is loose enough that only
    // the linear-in-triangles regression trips it.
    bake::BakeParams p = aoParams();
    p.aoSamples = 16;  // the copy, not the rays, is what this measures

    const Mesh low = lowPlane();
    const Mesh light = lowPlane();      // 2 triangles
    const Mesh heavy = densePlane(96);  // 9216 quads -> ~18k triangles

    const long long lightMs = aoBakeMs(low, light, p);
    const long long heavyMs = aoBakeMs(low, heavy, p);
    CHECK(heavyMs < 12 * lightMs + 100);
}

TEST_CASE("AO bake honors cooperative cancellation") {
    // AO shades its texels in parallel; a cancelled bake still has to report it.
    const Mesh low = lowPlane();
    const Mesh high = highWithRidge();
    CancelToken cancel;
    cancel.requestCancel();
    const bake::BakeResult r =
        bake::bake(low, high, bake::BakeMap::AmbientOcclusion, aoParams(), nullptr, &cancel);
    REQUIRE(r.cancelled);
}

TEST_CASE("AO self-occlusion of a flat mesh against itself stays open") {
    const Mesh low = lowPlane();
    const Mesh same = lowPlane();
    const bake::BakeResult r = bake::bake(low, same, bake::BakeMap::AmbientOcclusion, aoParams());
    REQUIRE_FALSE(r.image.pixels.empty());
    // Projection hits the coincident surface at ~0 distance -> fully open.
    CHECK(aoAt(r.image, 0.5f, 0.5f) > 0.9f);
}
