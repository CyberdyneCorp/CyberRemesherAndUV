#include <doctest.h>

#include <algorithm>
#include <array>
#include <atomic>
#include <chrono>
#include <cmath>
#include <cstddef>
#include <cstdlib>
#include <exception>
#include <iostream>
#include <map>
#include <new>
#include <sstream>
#include <streambuf>
#include <string>
#include <thread>
#include <utility>
#include <vector>

#ifndef _WIN32
#include <csignal>
#endif

// mallinfo2() (glibc 2.33+) is how the per-call retention case reads the live
// heap; without it that case cannot measure and is compiled out.
#if defined(CYBER_TESTS_HAVE_QUADCOVER) && defined(__GLIBC__) && \
    (__GLIBC__ > 2 || (__GLIBC__ == 2 && __GLIBC_MINOR__ >= 33))
#include <malloc.h>
#define CYBER_TESTS_HAVE_MALLINFO2 1
#endif

#include "cyber/core/mesh.hpp"
#include "cyber/quadrangulate/quadcover_extractor.hpp"

using cyber::EdgeId;
using cyber::FaceId;
using cyber::Index;
using cyber::Mesh;
using cyber::Vec2;
using cyber::Vec3;
using cyber::VertexId;
namespace remesh = cyber::remesh;

namespace {

// Two triangles forming a unit quad in the z = 0 plane.
Mesh makeTwoTri() {
    std::vector<Vec3> p = {{0, 0, 0}, {1, 0, 0}, {1, 1, 0}, {0, 1, 0}};
    std::vector<std::vector<Index>> f = {{0, 1, 2}, {0, 2, 3}};
    return Mesh::fromIndexed(p, f);
}

// A closed UV sphere centred at (dx, 0, 0), appended to p/f.
void appendSphere(std::vector<Vec3>& p, std::vector<std::vector<Index>>& f, float dx, int rings,
                  int segments) {
    const Index base = static_cast<Index>(p.size());
    p.push_back({dx, 0, 1});
    for (int r = 1; r < rings; ++r) {
        const float phi = 3.14159265f * static_cast<float>(r) / static_cast<float>(rings);
        for (int s = 0; s < segments; ++s) {
            const float th =
                2.0f * 3.14159265f * static_cast<float>(s) / static_cast<float>(segments);
            p.push_back(
                {dx + std::sin(phi) * std::cos(th), std::sin(phi) * std::sin(th), std::cos(phi)});
        }
    }
    p.push_back({dx, 0, -1});
    const Index south = static_cast<Index>(p.size() - 1);
    const auto ring = [&](int r, int s) {
        return base + static_cast<Index>(1 + (r - 1) * segments + (s % segments));
    };
    for (int s = 0; s < segments; ++s) {
        f.push_back({base, ring(1, s), ring(1, s + 1)});
    }
    for (int r = 1; r + 1 < rings; ++r) {
        for (int s = 0; s < segments; ++s) {
            f.push_back({ring(r, s), ring(r + 1, s), ring(r + 1, s + 1)});
            f.push_back({ring(r, s), ring(r + 1, s + 1), ring(r, s + 1)});
        }
    }
    for (int s = 0; s < segments; ++s) {
        f.push_back({south, ring(rings - 1, s + 1), ring(rings - 1, s)});
    }
}

// A closed UV sphere — a well-formed input for the seamless-UV solver.
Mesh makeSphere(int rings = 16, int segments = 24) {
    std::vector<Vec3> p;
    std::vector<std::vector<Index>> f;
    appendSphere(p, f, 0.0f, rings, segments);
    return Mesh::fromIndexed(p, f);
}

// Two disjoint spheres. A two-island input, so the vendored solver parameterizes
// the islands on its worker pool and traces its progress from THOSE threads, not
// only from the thread that called in.
Mesh makeSpherePair() {
    std::vector<Vec3> p;
    std::vector<std::vector<Index>> f;
    appendSphere(p, f, 0.0f, 16, 24);
    appendSphere(p, f, 4.0f, 16, 24);
    return Mesh::fromIndexed(p, f);
}

// A unit cube (6 quad faces) — every one of its 12 edges is a sharp 90-degree
// crease, so its crease-edge fraction is 1.0.
Mesh makeCube() {
    const std::vector<Vec3> p = {{0, 0, 0}, {1, 0, 0}, {1, 1, 0}, {0, 1, 0},
                                 {0, 0, 1}, {1, 0, 1}, {1, 1, 1}, {0, 1, 1}};
    const std::vector<std::vector<Index>> f = {{0, 3, 2, 1}, {4, 5, 6, 7}, {0, 1, 5, 4},
                                               {2, 3, 7, 6}, {1, 2, 6, 5}, {3, 0, 4, 7}};
    return Mesh::fromIndexed(p, f);
}

std::size_t aliveFaces(const Mesh& mesh) {
    std::size_t n = 0;
    for (Index i = 0; i < mesh.faceCapacity(); ++i) {
        if (mesh.isAlive(FaceId{i})) {
            ++n;
        }
    }
    return n;
}

// Build a SeamlessUv directly for a flat N x N grid of quad cells in the z = 0
// plane. Vertex (i, j) sits at world (i, j, 0) and carries UV = (i, j), so the
// integer isolines u,v in 0..N land exactly on the grid lines. Each cell is split
// into two triangles. Feeding this to extractIsolineQuads must recover the N x N
// quad grid: the isoline tracer's crossings are the grid vertices, valence 4 by
// construction on the interior.
remesh::SeamlessUv makeFlatGridUv(int n) {
    remesh::SeamlessUv uv;
    const auto vid = [&](int i, int j) { return static_cast<Index>(j * (n + 1) + i); };
    for (int j = 0; j <= n; ++j) {
        for (int i = 0; i <= n; ++i) {
            uv.vertices.push_back(Vec3{static_cast<float>(i), static_cast<float>(j), 0.0f});
        }
    }
    const auto uvOf = [](Index v, int n1) {
        const int i = static_cast<int>(v) % (n1 + 1);
        const int j = static_cast<int>(v) / (n1 + 1);
        return Vec2{static_cast<float>(i), static_cast<float>(j)};
    };
    const auto addTri = [&](Index a, Index b, Index c) {
        uv.triangles.push_back({a, b, c});
        uv.triangleUv.push_back({uvOf(a, n), uvOf(b, n), uvOf(c, n)});
    };
    for (int j = 0; j < n; ++j) {
        for (int i = 0; i < n; ++i) {
            const Index v00 = vid(i, j);
            const Index v10 = vid(i + 1, j);
            const Index v11 = vid(i + 1, j + 1);
            const Index v01 = vid(i, j + 1);
            addTri(v00, v10, v11);
            addTri(v00, v11, v01);
        }
    }
    uv.valid = true;
    return uv;
}

// Valence (incident alive faces) of a vertex, and whether it touches a boundary
// edge (an edge with a single incident face).
struct VertexInfo {
    std::size_t faces = 0;
    bool boundary = false;
};
VertexInfo vertexInfo(const Mesh& mesh, VertexId v) {
    VertexInfo info;
    info.faces = mesh.vertexFaces(v).size();
    for (const EdgeId e : mesh.vertexEdges(v)) {
        if (mesh.isBoundaryEdge(e)) {
            info.boundary = true;
        }
    }
    return info;
}

}  // namespace

// M3 contract: makeQuadCoverQuadrangulator yields a real IQuadrangulator. When the
// seamless-UV harness (CYBER_QUADCOVER_CLI) is unavailable it must FAIL CLEANLY —
// report failure with a reason and leave the mesh exactly as-is — so the pipeline
// degrades safely. When it succeeds it must have rewritten the mesh with faces. The
// invariant that matters either way: on the failure path the input is untouched.
TEST_CASE("quad-cover quadrangulator degrades cleanly and never corrupts on failure") {
    auto q = remesh::makeQuadCoverQuadrangulator();
    REQUIRE(q != nullptr);
    CHECK(q->name() == "quad-cover");

    Mesh mesh = makeTwoTri();
    const std::size_t facesBefore = aliveFaces(mesh);

    auto outcome = q->quadrangulate(mesh, 1.0f, nullptr, nullptr);

    CHECK_FALSE(outcome.cancelled);
    if (outcome.success) {
        // Harness available: the mesh was replaced with an extracted quad mesh.
        CHECK(aliveFaces(mesh) > 0);
    } else {
        // Harness absent (or extraction declined): a reason is reported and the input
        // triangle island is left exactly as it was.
        CHECK_FALSE(outcome.failureReason.empty());
        CHECK(aliveFaces(mesh) == facesBefore);
    }
}

// The isoline tracer (Milestone 2) is still a stub: an invalid UV yields no quads.
TEST_CASE("quad-cover isoline extractor returns empty for an invalid UV") {
    Mesh mesh = makeTwoTri();
    const remesh::SeamlessUv invalid;  // valid == false
    auto out = remesh::extractIsolineQuads(mesh, invalid);
    CHECK(out.vertices.empty());
    CHECK(out.quads.empty());
}

namespace {

// Max number of faces sharing any single undirected edge — must stay <= 2 for a manifold
// re-partition. Also counts how many faces are non-quad.
struct CapStats {
    std::size_t maxEdgeFaces = 0;
    std::size_t nonQuad = 0;
};
CapStats capStats(const std::vector<std::vector<std::size_t>>& faces) {
    std::map<std::pair<std::size_t, std::size_t>, std::size_t> edge;
    CapStats s;
    for (const auto& f : faces) {
        if (f.size() != 4) {
            ++s.nonQuad;
        }
        for (std::size_t i = 0; i < f.size(); ++i) {
            std::size_t a = f[i];
            std::size_t b = f[(i + 1) % f.size()];
            if (a > b) {
                std::swap(a, b);
            }
            ++edge[{a, b}];
        }
    }
    for (const auto& [e, n] : edge) {
        (void)e;
        s.maxEdgeFaces = std::max(s.maxEdgeFaces, n);
    }
    return s;
}

}  // namespace

// Cap elimination: the tracer's non-quad caps must become quads before the pipeline's
// pure-quad subdivision (each non-quad n-gon would otherwise Catmull-Clark into a
// valence-n fan-centre irregular). The pass re-partitions over the SAME vertex set and
// must stay manifold (no edge in > 2 faces).
TEST_CASE("quad-cover cap elimination: adjacent triangles merge into a quad") {
    std::vector<Vec3> verts = {{0, 0, 0}, {1, 0, 0}, {1, 1, 0}, {0, 1, 0}};
    // A quad split by its diagonal into two triangles (the classic residual cap pair).
    std::vector<std::vector<std::size_t>> faces = {{0, 1, 2}, {0, 2, 3}};
    const std::size_t vBefore = verts.size();
    remesh::eliminateNonQuadCaps(verts, faces);
    CHECK(verts.size() == vBefore);  // no vertices added
    CHECK(faces.size() == 1);        // two triangles -> one quad
    CHECK(faces.front().size() == 4);
    CHECK(capStats(faces).nonQuad == 0);
    CHECK(capStats(faces).maxEdgeFaces <= 2);  // still manifold
}

TEST_CASE("quad-cover cap elimination: an even n-gon splits into quads") {
    // Regular hexagon in the z = 0 plane.
    std::vector<Vec3> verts;
    for (int i = 0; i < 6; ++i) {
        const float a = 2.0f * 3.14159265f * static_cast<float>(i) / 6.0f;
        verts.push_back({std::cos(a), std::sin(a), 0.0f});
    }
    std::vector<std::vector<std::size_t>> faces = {{0, 1, 2, 3, 4, 5}};
    const std::size_t vBefore = verts.size();
    remesh::eliminateNonQuadCaps(verts, faces);
    CHECK(verts.size() == vBefore);
    CHECK(faces.size() == 2);  // hexagon -> two quads
    CHECK(capStats(faces).nonQuad == 0);
    CHECK(capStats(faces).maxEdgeFaces <= 2);
}

TEST_CASE("quad-cover cap elimination: a lone quad is left untouched") {
    std::vector<Vec3> verts = {{0, 0, 0}, {1, 0, 0}, {1, 1, 0}, {0, 1, 0}};
    std::vector<std::vector<std::size_t>> faces = {{0, 1, 2, 3}};
    remesh::eliminateNonQuadCaps(verts, faces);
    CHECK(faces.size() == 1);
    CHECK(faces.front().size() == 4);
}

// Milestone 1: computeSeamlessUv obtains a seamless integer-grid UV out-of-process
// from AutoRemesher's Geogram quad_cover when CYBER_QUADCOVER_CLI points at a built
// autoremesher_cli. The UV must be genuinely seamless — the integer-jump residual
// across every interior edge is ~0. Without the harness (env unset / build absent) it
// degrades cleanly to an invalid UV, so the test is a no-op there.
TEST_CASE("quad-cover M1: harness seamless UV has zero integer-jump residual") {
    const Mesh sphere = makeSphere();
    const remesh::SeamlessUv uv = remesh::computeSeamlessUv(sphere, 0.15f);
    if (!uv.valid) {
        CHECK(uv.triangles.empty());  // harness unavailable -> clean degrade
        return;
    }
    CHECK(uv.triangles.size() == uv.triangleUv.size());
    CHECK(uv.vertices.size() > 0);
    CHECK(remesh::seamlessUvResidual(uv) < 1e-3);  // seamless by construction
}

namespace {

// Captures everything the guarded scope writes to std::cout / std::cerr.
class ConsoleCapture {
public:
    ConsoleCapture()
        : savedOut_(std::cout.rdbuf(out_.rdbuf())), savedErr_(std::cerr.rdbuf(err_.rdbuf())) {}
    ~ConsoleCapture() {
        std::cout.rdbuf(savedOut_);
        std::cerr.rdbuf(savedErr_);
    }
    ConsoleCapture(const ConsoleCapture&) = delete;
    ConsoleCapture& operator=(const ConsoleCapture&) = delete;
    std::string out() const { return out_.str(); }
    std::string err() const { return err_.str(); }

private:
    std::ostringstream out_;
    std::ostringstream err_;
    std::streambuf* savedOut_;
    std::streambuf* savedErr_;
};

#ifndef _WIN32
// The process-global state the vendored Geogram solver used to take over.
const int kWatchedSignals[] = {SIGSEGV, SIGILL, SIGBUS, SIGFPE, SIGINT};

struct HostState {
    std::array<struct sigaction, sizeof(kWatchedSignals) / sizeof(kWatchedSignals[0])> signals{};
    std::terminate_handler terminate = nullptr;
    std::new_handler newHandler = nullptr;
    std::string lcNumeric;
};

HostState snapshotHostState() {
    HostState state;
    for (std::size_t i = 0; i < state.signals.size(); ++i) {
        ::sigaction(kWatchedSignals[i], nullptr, &state.signals[i]);
    }
    state.terminate = std::get_terminate();
    state.newHandler = std::get_new_handler();
    const char* lc = std::getenv("LC_NUMERIC");
    state.lcNumeric = lc != nullptr ? lc : "";
    return state;
}

bool sameDisposition(const struct sigaction& a, const struct sigaction& b) {
    if (a.sa_flags != b.sa_flags) {
        return false;
    }
    return (a.sa_flags & SA_SIGINFO) != 0 ? a.sa_sigaction == b.sa_sigaction
                                          : a.sa_handler == b.sa_handler;
}
#endif

#ifdef CYBER_TESTS_HAVE_MALLINFO2
// Bytes the process holds in in-use main-arena blocks. Freed memory drops out of
// it whether or not the allocator hands the pages back, which is what makes it a
// RETENTION measure rather than an RSS reading.
std::size_t liveHeapBytes() { return mallinfo2().uordblks; }
#endif

}  // namespace

// Host-process hygiene. The vendored Geogram/AutoRemesher solver behind the default
// quad-cover path used to hijack the host's SIGSEGV/SIGILL/SIGBUS/SIGFPE handlers,
// reset SIGINT, replace std::terminate and std::new_handler, rewrite LC_NUMERIC and
// trace hundreds of lines to std::cerr — a DCC plugin lost its crash reporter and got
// spammed the moment a user remeshed. A solve must leave every one of those exactly as
// it found it. (The handler half only bites on the FIRST solve in a process, so the
// fresh-process check lives in python/cyberremesh/tests/test_host_process_hygiene.py;
// the console half is per-call and is what this case guards everywhere.)
TEST_CASE("quad-cover seamless UV leaves the host's console and handlers alone") {
    const Mesh sphere = makeSphere();
#ifndef _WIN32
    const char* lcBefore = std::getenv("LC_NUMERIC");
    const std::string lcRestore = lcBefore != nullptr ? lcBefore : "";
    ::setenv("LC_NUMERIC", "en_US.UTF-8", 1);  // sentinel: a rewrite must be visible
    const HostState before = snapshotHostState();
#endif

    std::string outText;
    std::string errText;
    {
        const ConsoleCapture capture;
        const remesh::SeamlessUv uv = remesh::computeSeamlessUv(sphere, 0.15f);
        outText = capture.out();
        errText = capture.err();
        CHECK(uv.triangles.size() == uv.triangleUv.size());
    }
    CHECK(errText.empty());
    CHECK(outText.empty());

#ifndef _WIN32
    const HostState after = snapshotHostState();
    for (std::size_t i = 0; i < before.signals.size(); ++i) {
        CHECK(sameDisposition(before.signals[i], after.signals[i]));
    }
    CHECK(before.terminate == after.terminate);
    CHECK(before.newHandler == after.newHandler);
    CHECK(after.lcNumeric == "en_US.UTF-8");
    if (lcRestore.empty()) {
        ::unsetenv("LC_NUMERIC");
    } else {
        ::setenv("LC_NUMERIC", lcRestore.c_str(), 1);
    }
#endif
}

// The other half of the console contract: silencing the vendored solver must not
// silence the HOST. The solve used to point the process-global std::cout/std::cerr
// at a null sink for its whole duration, so an embedder logging from its own thread
// lost every line for as long as a remesh ran. What the host writes must still reach
// the buffer the host installed, while the solver's own chatter — traced from the
// worker threads it farms the islands out to as well as from the calling thread —
// still reaches nothing.
TEST_CASE("quad-cover seamless UV silences the solver without silencing the host") {
    const Mesh pair = makeSpherePair();  // two islands -> the solver's worker pool traces too
    const std::string hostLine = "host-log-line\n";
    constexpr int kMaxHostLines = 4000;  // bound: a regression must fail, never spin

    std::atomic<bool> solving{true};
    std::atomic<int> written{0};
    std::string outText;
    std::string errText;
    {
        const ConsoleCapture capture;
        std::thread hostLogger([&] {
            do {
                std::cout << hostLine;
                written.fetch_add(1);
                std::this_thread::sleep_for(std::chrono::milliseconds(1));
            } while (solving.load() && written.load() < kMaxHostLines);
        });
        const remesh::SeamlessUv uv = remesh::computeSeamlessUv(pair, 0.15f);
        solving.store(false);
        hostLogger.join();
        outText = capture.out();
        errText = capture.err();
        CHECK(uv.triangles.size() == uv.triangleUv.size());
    }

    const int lines = written.load();
    REQUIRE(lines > 0);
    std::size_t delivered = 0;
    std::string residue;
    for (std::size_t at = 0; at < outText.size();) {
        const std::size_t hit = outText.find(hostLine, at);
        if (hit == std::string::npos) {
            residue += outText.substr(at);
            break;
        }
        residue += outText.substr(at, hit - at);
        at = hit + hostLine.size();
        ++delivered;
    }
    CHECK(delivered == static_cast<std::size_t>(lines));  // not one host line swallowed
    CHECK(residue.empty());                               // and no solver chatter leaked
    CHECK(errText.empty());
}

#ifdef CYBER_TESTS_HAVE_MALLINFO2
// Declared by hand instead of including <geogram/bibliography/bibliography.h>:
// the strict C++20 test build deliberately never sees the vendored C++14 headers
// (that isolation is the whole point of the cyber_quadcover_solver target).
namespace GEO::Biblio {
void cite(const char* ref, const char* file, int line, const char* function, const char* info);
}  // namespace GEO::Biblio

// The third process-global Geogram takeover: its citation registry is a static
// vector that geo_cite() appends to with no dedup and no cap, once per spatial
// sort inside a solve, and nothing in this tree ever consumed or cleared it — so
// a host that remeshes for hours kept every record of every solve, invisible to
// LeakSanitizer because the memory stays reachable from the static. A solve must
// leave the registry as empty as it found it.
//
// Measured through the registry's OWN records rather than the solve's total heap
// churn: each iteration seeds a known, large number of citations before solving,
// so the leak this guards against is ~2 MB per iteration while the rest of a
// solve's per-call residue is a few kB. Bounded by construction — kIters solves
// of a small sphere, and the unfixed peak is ~40 MB, so a regression fails on the
// assertion instead of exhausting CI.
TEST_CASE("quad-cover seamless UV leaves no citation records behind") {
    const Mesh sphere = makeSphere(8, 10);
    const std::string info(256, 'x');  // big enough that each record is heap, not SSO
    constexpr int kIters = 20;
    constexpr int kWarmIters = 10;  // by here every one-shot cache is populated
    constexpr int kCitesPerIter = 4000;
    constexpr std::size_t kAllowedGrowth = std::size_t{4} << 20;

    // cite() reads Geogram's CmdLine, so the runtime has to be up before the
    // first record: one solve is what brings it up (GEO::initialize()).
    REQUIRE(remesh::computeSeamlessUv(sphere, 0.3f).valid);

    // A sanitizer replaces the allocator wholesale, so glibc's main arena stays
    // empty and mallinfo2 reports 0 for every field. There is nothing to measure
    // then — the retention this case guards is real, but only a build running on
    // glibc's own allocator can see it, so say so rather than failing the
    // sanitizer lane on an instrument that is not connected.
    if (liveHeapBytes() == 0) {
        MESSAGE("skipped: the allocator is replaced (sanitizer build), mallinfo2 reads 0");
        return;
    }

    std::size_t warm = 0;
    std::size_t last = 0;
    for (int i = 1; i <= kIters; ++i) {
        for (int c = 0; c < kCitesPerIter; ++c) {
            GEO::Biblio::cite("CYBER:RETENTION", "test_quadcover_extractor.cpp", __LINE__,
                              "citationRetention()", info.c_str());
        }
        const remesh::SeamlessUv uv = remesh::computeSeamlessUv(sphere, 0.3f);
        REQUIRE(uv.valid);  // a declined solve would never reach the reclaimer
        if (i == kWarmIters) {
            warm = liveHeapBytes();
        }
        last = liveHeapBytes();
    }
    REQUIRE(warm > 0);
    CHECK(last < warm + kAllowedGrowth);
}
#endif  // CYBER_TESTS_HAVE_MALLINFO2

// Milestone 2 PRIMARY checkpoint: the isoline tracer on a flat integer-grid UV must
// recover a clean N x N quad grid. This exercises the tracer core deterministically
// (no external harness): extractConnections -> extractEdges -> extractMesh must find
// the grid cells as transversal-crossing quads. The result is an open disk, so its
// perimeter is a legitimate boundary; every INTERIOR vertex must be valence 4 (zero
// irregular), all faces must be quads, and the half-edge structure must be sound.
TEST_CASE("creaseEdgeFraction routes sharp CAD to native, keeps smooth meshes vendored") {
    // The discriminator behind computeSeamlessUv's CAD routing: a sharp cube sits
    // above the 2% routing threshold (-> feature-aware native solver), a smooth
    // sphere far below it (-> vendored Geogram path). This is what keeps fandisk
    // routed while the organic corpus stays byte-identical on the vendored path.
    const float smooth = remesh::creaseEdgeFraction(makeSphere(), 45.0f);
    const float sharp = remesh::creaseEdgeFraction(makeCube(), 45.0f);
    REQUIRE(smooth < 0.02f);
    REQUIRE(sharp > 0.02f);
    REQUIRE(sharp > smooth);
}

TEST_CASE("quad-cover M2: flat integer-grid UV extracts a clean quad grid") {
    const int n = 6;
    const remesh::SeamlessUv uv = makeFlatGridUv(n);
    Mesh dummy = makeTwoTri();  // mesh param is ignored by the extractor

    const remesh::IsolineQuadMesh out = remesh::extractIsolineQuads(dummy, uv);

    REQUIRE_FALSE(out.quads.empty());
    for (const auto& face : out.quads) {
        CHECK(face.size() == 4);
    }

    // Rebuild as a cyber::Mesh to check topology.
    std::vector<std::vector<Index>> faces;
    faces.reserve(out.quads.size());
    for (const auto& q : out.quads) {
        std::vector<Index> f;
        f.reserve(q.size());
        for (const std::size_t v : q) {
            f.push_back(static_cast<Index>(v));
        }
        faces.push_back(std::move(f));
    }
    const Mesh mesh = Mesh::fromIndexed(out.vertices, faces);

    // Structural half-edge invariants must all hold (fromIndexed is non-manifold
    // safe, so this catches any face that references a stale/duplicate vertex).
    CHECK(mesh.validate().empty());

    // Interior vertices (not touching a boundary edge) must be regular (valence 4);
    // zero interior irregular vertices is the whole point of transversal crossings.
    std::size_t interior = 0;
    std::size_t interiorIrregular = 0;
    for (Index i = 0; i < mesh.vertexCapacity(); ++i) {
        const VertexId v{i};
        if (!mesh.isAlive(v)) {
            continue;
        }
        const VertexInfo info = vertexInfo(mesh, v);
        if (info.boundary) {
            continue;
        }
        ++interior;
        if (info.faces != 4) {
            ++interiorIrregular;
        }
    }
    CHECK(interior > 0);
    CHECK(interiorIrregular == 0);

    // A clean 6x6 grid has 36 quads and 25 interior vertices; allow no irregular
    // interior but don't over-fit the exact face count (boundary handling may add a
    // few perimeter faces). The grid must at least cover its interior.
    CHECK(interior == static_cast<std::size_t>((n - 1) * (n - 1)));
}

// Milestone 3 (needs the M1 harness): a real seamless UV on a CLOSED sphere must
// extract a clean, near-all-quad mesh with QuadriFlow-class irregular fraction.
// No-op when the harness is unavailable.
//
// This is the boundary-aware gate at work: extractIsolineQuads detects the isotropic
// mesh is closed and runs the closed-surface graph cleanup (simplifyGraph / fixHoles /
// collapse*), which merges the raw isoline oversampling into clean quad cells. With the
// gate a UV sphere goes from ~350 non-quad n-gons (core only) to <1% non-quad and ~1%
// interior irregular — matching QuadriFlow's 1-4%.
TEST_CASE("quad-cover M3: harness sphere UV extracts a clean closed quad mesh") {
    const Mesh sphere = makeSphere();
    const remesh::SeamlessUv uv = remesh::computeSeamlessUv(sphere, 0.15f);
    if (!uv.valid) {
        CHECK(uv.triangles.empty());  // harness unavailable -> clean degrade
        return;
    }
    const remesh::IsolineQuadMesh out = remesh::extractIsolineQuads(sphere, uv);
    REQUIRE_FALSE(out.quads.empty());
    CHECK(out.vertices.size() > 0);

    std::size_t nonQuad = 0;
    for (const auto& face : out.quads) {
        CHECK(face.size() >= 3);  // every polygon is at least a triangle
        if (face.size() != 4) {
            ++nonQuad;
        }
    }
    // A closed sphere has no true boundary, so the cleanup should leave essentially
    // only quads (allow a tiny handful of unavoidable cap polygons).
    CHECK(nonQuad <= out.quads.size() / 20);

    // Rebuild as a mesh (fromIndexed is non-manifold safe) and check it is sound and
    // low-defect on the interior.
    std::vector<std::vector<Index>> faces;
    faces.reserve(out.quads.size());
    for (const auto& q : out.quads) {
        std::vector<Index> f;
        for (const std::size_t v : q) {
            f.push_back(static_cast<Index>(v));
        }
        faces.push_back(std::move(f));
    }
    const Mesh mesh = Mesh::fromIndexed(out.vertices, faces);
    CHECK(mesh.faceCount() > 0);
    CHECK(mesh.validate().empty());

    std::size_t interior = 0;
    std::size_t interiorIrregular = 0;
    for (Index i = 0; i < mesh.vertexCapacity(); ++i) {
        const VertexId v{i};
        if (!mesh.isAlive(v)) {
            continue;
        }
        const VertexInfo info = vertexInfo(mesh, v);
        if (info.boundary) {
            continue;
        }
        ++interior;
        if (info.faces != 4) {
            ++interiorIrregular;
        }
    }
    REQUIRE(interior > 0);
    // QuadriFlow-class: well under 5% of interior vertices are irregular.
    const double irregularFraction =
        static_cast<double>(interiorIrregular) / static_cast<double>(interior);
    CHECK(irregularFraction < 0.05);
}

namespace {

// A CLOSED flat-torus SeamlessUv: an n x n grid of quad cells whose (i, j) vertex
// sits on a geometric torus and carries UV = (i, j), with the wraparound cells
// using UV = n (an integer translation of 0, so the grid is exactly seamless).
// The cells in [holeI, holeI + holeSize) x [holeJ, holeJ + holeSize) are omitted,
// leaving an extraction-side hole whose rim is NOT input boundary of a "real" open
// surface: the surface is closed everywhere else, so the boundary-fraction gate
// routes it through the closed-surface cleanup and fixHoles sees one genuine loop
// of 4 * holeSize edges.
remesh::SeamlessUv makeTorusGridUvWithHole(int n, int holeI, int holeJ, int holeSize) {
    remesh::SeamlessUv uv;
    const float kPi = 3.14159265358979f;
    const float R = 2.0f;
    const float r = 0.5f;
    const auto vid = [&](int i, int j) {
        return static_cast<Index>(((j % n + n) % n) * n + ((i % n + n) % n));
    };
    for (int j = 0; j < n; ++j) {
        for (int i = 0; i < n; ++i) {
            const float u = 2.0f * kPi * static_cast<float>(i) / static_cast<float>(n);
            const float v = 2.0f * kPi * static_cast<float>(j) / static_cast<float>(n);
            uv.vertices.push_back(Vec3{(R + r * std::cos(v)) * std::cos(u),
                                       (R + r * std::cos(v)) * std::sin(u), r * std::sin(v)});
        }
    }
    // Per-corner UVs are the UNWRAPPED integers of the cell, so each triangle's UV
    // is affine and the wraparound edge differs from column/row 0 by exactly n.
    const auto addTri = [&](std::array<int, 2> a, std::array<int, 2> b, std::array<int, 2> c) {
        uv.triangles.push_back({vid(a[0], a[1]), vid(b[0], b[1]), vid(c[0], c[1])});
        uv.triangleUv.push_back({Vec2{static_cast<float>(a[0]), static_cast<float>(a[1])},
                                 Vec2{static_cast<float>(b[0]), static_cast<float>(b[1])},
                                 Vec2{static_cast<float>(c[0]), static_cast<float>(c[1])}});
    };
    for (int j = 0; j < n; ++j) {
        for (int i = 0; i < n; ++i) {
            if (i >= holeI && i < holeI + holeSize && j >= holeJ && j < holeJ + holeSize) {
                continue;  // the manufactured hole
            }
            addTri({i, j}, {i + 1, j}, {i + 1, j + 1});
            addTri({i, j}, {i + 1, j + 1}, {i, j + 1});
        }
    }
    uv.valid = true;
    return uv;
}

// Rebuild an IsolineQuadMesh as a cyber::Mesh and count its boundary edges.
Mesh rebuildMesh(const remesh::IsolineQuadMesh& out) {
    std::vector<std::vector<Index>> faces;
    faces.reserve(out.quads.size());
    for (const auto& q : out.quads) {
        std::vector<Index> f;
        f.reserve(q.size());
        for (const std::size_t v : q) {
            f.push_back(static_cast<Index>(v));
        }
        faces.push_back(std::move(f));
    }
    return Mesh::fromIndexed(out.vertices, faces);
}

std::size_t boundaryEdgeCount(const Mesh& mesh) {
    std::size_t n = 0;
    for (Index i = 0; i < mesh.edgeCapacity(); ++i) {
        const EdgeId e{i};
        if (mesh.isAlive(e) && mesh.isBoundaryEdge(e)) {
            ++n;
        }
    }
    return n;
}

}  // namespace

// Hole-fill policy (remeshing-parameters spec, holeFillMaxBoundary): boundary loops
// with at most `holeFillMaxBoundary` edges are closed with quads during extraction,
// LONGER loops stay open. A 2x2 block of cells removed from a closed flat torus
// leaves exactly one 8-edge loop: the default limit (64) must close it, a limit of
// 4 must leave it open — not a hard-coded 65 as in the AutoRemesher reference.
TEST_CASE("quad-cover hole-fill policy: loops longer than holeFillMaxBoundary stay open") {
    const int n = 8;
    const remesh::SeamlessUv uv = makeTorusGridUvWithHole(n, 3, 3, 2);
    Mesh dummy = makeTwoTri();  // mesh param is ignored by the extractor

    // Default policy (64): the 8-edge loop is within the limit and gets filled, so
    // the extracted torus is closed again (zero boundary edges).
    const remesh::IsolineQuadMesh filled = remesh::extractIsolineQuads(dummy, uv);
    REQUIRE_FALSE(filled.quads.empty());
    for (const auto& face : filled.quads) {
        CHECK(face.size() == 4);
    }
    const Mesh filledMesh = rebuildMesh(filled);
    CHECK(filledMesh.validate().empty());
    CHECK(boundaryEdgeCount(filledMesh) == 0);

    // Tight policy (4): the 8-edge loop exceeds the limit and must stay open.
    const remesh::IsolineQuadMesh open = remesh::extractIsolineQuads(dummy, uv, 4);
    REQUIRE_FALSE(open.quads.empty());
    const Mesh openMesh = rebuildMesh(open);
    CHECK(openMesh.validate().empty());
    CHECK(boundaryEdgeCount(openMesh) == 8);
    // The skipped fill is the only difference: the open variant has fewer faces.
    CHECK(open.quads.size() < filled.quads.size());

    // Filling disabled outright (< 3): identical open result.
    const remesh::IsolineQuadMesh none = remesh::extractIsolineQuads(dummy, uv, 0);
    CHECK(none.quads.size() == open.quads.size());
    CHECK(boundaryEdgeCount(rebuildMesh(none)) == 8);
}

// The factory clamps its construction arguments to the documented parameter
// ranges (remeshing-parameters spec, "Validation at every entry point"): these
// values drive the solve directly and never pass through remesh()'s validation,
// so an out-of-range adaptivity used to produce a mesh that no in-range run
// could reproduce while the caller reported it as clamped.
TEST_CASE("quad-cover quadrangulator clamps out-of-range construction parameters") {
    auto outOfRange = remesh::makeQuadCoverQuadrangulator(40, 7.0f, 64, 90.0f);
    auto atMaximum = remesh::makeQuadCoverQuadrangulator(40, 1.0f, 64, 90.0f);
    REQUIRE(outOfRange != nullptr);
    REQUIRE(atMaximum != nullptr);

    Mesh clamped = makeSphere();
    Mesh maximum = makeSphere();
    const auto a = outOfRange->quadrangulate(clamped, 0.15f, nullptr, nullptr);
    const auto b = atMaximum->quadrangulate(maximum, 0.15f, nullptr, nullptr);

    REQUIRE(a.success == b.success);
    REQUIRE(aliveFaces(clamped) == aliveFaces(maximum));
    REQUIRE(clamped.vertexCount() == maximum.vertexCount());
    // One aggregated check: a per-vertex assertion would flood the log.
    float maxDelta = 0.0f;
    for (Index i = 0; i < clamped.vertexCapacity(); ++i) {
        if (!clamped.isAlive(VertexId{i})) {
            continue;
        }
        const Vec3 p = clamped.position(VertexId{i});
        const Vec3 q = maximum.position(VertexId{i});
        maxDelta =
            std::max({maxDelta, std::abs(p.x - q.x), std::abs(p.y - q.y), std::abs(p.z - q.z)});
    }
    CHECK(maxDelta == doctest::Approx(0.0f));
}

// Cooperative cancellation (remeshing-pipeline spec): a cancelled token makes the
// quadrangulator return cancelled=true and leave the input mesh untouched, before
// any solver work runs.
TEST_CASE("quad-cover quadrangulator: a cancelled token aborts without touching the mesh") {
    auto q = remesh::makeQuadCoverQuadrangulator();
    REQUIRE(q != nullptr);

    Mesh mesh = makeTwoTri();
    const std::size_t facesBefore = aliveFaces(mesh);
    const std::size_t vertsBefore = mesh.vertexCount();

    cyber::CancelToken cancel;
    cancel.requestCancel();
    const auto outcome = q->quadrangulate(mesh, 0.25f, nullptr, &cancel);

    CHECK(outcome.cancelled);
    CHECK_FALSE(outcome.success);
    CHECK(aliveFaces(mesh) == facesBefore);
    CHECK(mesh.vertexCount() == vertsBefore);
}
