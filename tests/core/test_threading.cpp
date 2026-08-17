#include <doctest.h>

#include <algorithm>
#include <cstddef>
#include <memory>
#include <mutex>
#include <thread>
#include <utility>
#include <vector>

#include "cyber/accel/backend.hpp"
#include "cyber/core/mesh.hpp"
#include "cyber/core/pipeline.hpp"
#include "cyber/core/threading.hpp"

// Host control over the worker fan-out (cyber::setMaxWorkerThreads). Before it
// there was none: both parallel loops read std::thread::hardware_concurrency()
// straight, so an AO bake on an interactive host ran at every core it could see
// and there was no in-process way to ask for less.
//
// Two things need pinning. The cap must actually reach both loops — measured
// structurally, by the CHUNKS a range is split into, not by wall clock. And it
// must not change a single output byte, which is the whole reason a host is
// allowed to move it at will.
namespace {

// Restores whatever cap was in force, so one case cannot leak into the next
// (doctest runs them in one process, and the cap is process-global).
class ScopedThreadCap {
public:
    explicit ScopedThreadCap(std::size_t cap) : m_previous(cyber::maxWorkerThreads()) {
        cyber::setMaxWorkerThreads(cap);
    }
    ~ScopedThreadCap() { cyber::setMaxWorkerThreads(m_previous); }
    ScopedThreadCap(const ScopedThreadCap&) = delete;
    ScopedThreadCap& operator=(const ScopedThreadCap&) = delete;

private:
    std::size_t m_previous;
};

std::size_t countChunks(std::size_t items) {
    const std::shared_ptr<cyber::accel::IBackend> backend = cyber::accel::defaultBackend();
    std::mutex mutex;
    std::size_t chunks = 0;
    backend->parallelFor(0, items, [&](std::size_t, std::size_t) {
        const std::lock_guard<std::mutex> lock(mutex);
        ++chunks;
    });
    return chunks;
}

// A ridged grid: enough vertices for the relax loops to fan out, and enough
// shape that the pipeline has real work to do.
cyber::Mesh ridgedGrid(int n) {
    std::vector<cyber::Vec3> positions;
    positions.reserve(static_cast<std::size_t>((n + 1) * (n + 1)));
    for (int y = 0; y <= n; ++y) {
        for (int x = 0; x <= n; ++x) {
            const float fx = static_cast<float>(x) / static_cast<float>(n);
            const float fy = static_cast<float>(y) / static_cast<float>(n);
            positions.push_back({fx, fy, 0.15f * (fx - 0.5f) * (fx - 0.5f) + 0.1f * fy * fy});
        }
    }
    std::vector<std::vector<cyber::Index>> faces;
    const auto at = [n](int x, int y) { return static_cast<cyber::Index>(y * (n + 1) + x); };
    for (int y = 0; y < n; ++y) {
        for (int x = 0; x < n; ++x) {
            faces.push_back({at(x, y), at(x + 1, y), at(x + 1, y + 1)});
            faces.push_back({at(x, y), at(x + 1, y + 1), at(x, y + 1)});
        }
    }
    return cyber::Mesh::fromIndexed(positions, faces);
}

std::vector<cyber::Vec3> livePositions(const cyber::Mesh& mesh) {
    std::vector<cyber::Vec3> out;
    for (cyber::Index vi = 0; vi < mesh.vertexCapacity(); ++vi) {
        const cyber::VertexId v{vi};
        if (mesh.isAlive(v)) {
            out.push_back(mesh.position(v));
        }
    }
    return out;
}

std::vector<std::vector<cyber::Index>> liveFaces(const cyber::Mesh& mesh) {
    std::vector<std::vector<cyber::Index>> out;
    for (cyber::Index fi = 0; fi < mesh.faceCapacity(); ++fi) {
        const cyber::FaceId f{fi};
        if (!mesh.isAlive(f)) {
            continue;
        }
        std::vector<cyber::Index> face;
        for (const cyber::VertexId v : mesh.faceVertices(f)) {
            face.push_back(v.value);
        }
        out.push_back(std::move(face));
    }
    return out;
}

}  // namespace

TEST_CASE("the worker cap defaults to off and round-trips") {
    REQUIRE(cyber::maxWorkerThreads() == 0);
    const ScopedThreadCap cap(3);
    REQUIRE(cyber::maxWorkerThreads() == 3);
    REQUIRE(cyber::workerThreadsFor(1000) == 3);
    cyber::setMaxWorkerThreads(0);
    REQUIRE(cyber::maxWorkerThreads() == 0);
}

TEST_CASE("workerThreadsFor never returns 0 and never exceeds the item count") {
    const ScopedThreadCap cap(8);
    REQUIRE(cyber::workerThreadsFor(0) == 1);
    REQUIRE(cyber::workerThreadsFor(1) == 1);
    REQUIRE(cyber::workerThreadsFor(5) == 5);
    REQUIRE(cyber::workerThreadsFor(5000) == 8);
}

TEST_CASE("the cap reaches the accel fan-out") {
    // Structural, not timed: a capped run must split the range into exactly
    // `cap` chunks. A cap the loop ignored would show up as more.
    constexpr std::size_t items = 100'000;
    {
        const ScopedThreadCap cap(1);
        REQUIRE(countChunks(items) == 1);
    }
    {
        const ScopedThreadCap cap(2);
        REQUIRE(countChunks(items) == 2);
    }
    // Uncapped, the fan-out is the machine's — only checked where there is more
    // than one core to fan out to.
    if (std::thread::hardware_concurrency() >= 2) {
        const ScopedThreadCap cap(0);
        REQUIRE(countChunks(items) >= 2);
    }
}

TEST_CASE("a capped remesh is byte-identical to an uncapped one") {
    // The promise the C ABI makes to a host that turns the cap down mid-session:
    // it buys CPU headroom, never a different mesh. The pipeline's relax loops
    // are the ones the cap resizes.
    const cyber::Mesh input = ridgedGrid(24);
    cyber::remesh::Parameters params;
    params.targetQuadCount = 400;

    cyber::remesh::PipelineResult uncapped;
    {
        const ScopedThreadCap cap(0);
        uncapped = cyber::remesh::remesh(input, params);
    }
    cyber::remesh::PipelineResult capped;
    {
        const ScopedThreadCap cap(1);
        capped = cyber::remesh::remesh(input, params);
    }

    REQUIRE(!livePositions(uncapped.mesh).empty());
    REQUIRE(livePositions(capped.mesh) == livePositions(uncapped.mesh));
    REQUIRE(liveFaces(capped.mesh) == liveFaces(uncapped.mesh));
}
