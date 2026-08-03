// Regional prescribed-boundary solve — tasks 1-3 (openspec add-weave-regional-solve).
//
// These assert the EXACT-LANDING properties (G1), which the task-0 spike measured
// as always-holding. They deliberately do NOT assert interface regularity: that is
// a coupled degree-constrained matching no local pass solves, it is reported rather
// than guaranteed, and it is split out as task 5.3a. See the change's design.md.

#include <doctest.h>

#include <algorithm>
#include <cmath>
#include <cstring>
#include <limits>
#include <map>
#include <set>
#include <vector>

#include "cyber/core/interface_conformance.hpp"
#include "cyber/core/isotropic.hpp"
#include "cyber/core/mesh.hpp"
#include "cyber/core/pipeline.hpp"
#include "cyber/core/quadrangulate.hpp"
#include "cyber/core/reference_surface.hpp"
#include "cyber/core/region_solve.hpp"
#include "cyber/quadrangulate/field_quadrangulator.hpp"
#include "cyber/retopo/boundary.hpp"

using cyber::EdgeId;
using cyber::FaceId;
using cyber::Index;
using cyber::Mesh;
using cyber::Vec3;
using cyber::VertexId;
namespace remesh = cyber::remesh;

namespace {

constexpr std::size_t kN = 6;

template <typename H>
Mesh makeGrid(std::size_t n, H height) {
    Mesh mesh;
    std::vector<std::vector<VertexId>> id(n + 1, std::vector<VertexId>(n + 1));
    for (std::size_t i = 0; i <= n; ++i) {
        for (std::size_t j = 0; j <= n; ++j) {
            const float u = 2.0f * static_cast<float>(i) / static_cast<float>(n) - 1.0f;
            const float v = 2.0f * static_cast<float>(j) / static_cast<float>(n) - 1.0f;
            id[i][j] = mesh.addVertex({u, v, height(u, v)});
        }
    }
    for (std::size_t i = 0; i < n; ++i) {
        for (std::size_t j = 0; j < n; ++j) {
            const VertexId ring[4] = {id[i][j], id[i + 1][j], id[i + 1][j + 1], id[i][j + 1]};
            mesh.addFace(ring);
        }
    }
    return mesh;
}

Mesh flatGrid() {
    return makeGrid(kN, [](float, float) { return 0.0f; });
}
Mesh domedGrid() {
    return makeGrid(kN, [](float u, float v) {
        return 0.6f * std::sqrt(std::fmax(0.0f, 1.2f - (u * u + v * v)));
    });
}

FaceId cell(std::size_t i, std::size_t j) { return FaceId{static_cast<Index>(i * kN + j)}; }

std::vector<FaceId> centreBlock() {
    std::vector<FaceId> faces;
    for (std::size_t i = 1; i <= 4; ++i) {
        for (std::size_t j = 1; j <= 4; ++j) {
            faces.push_back(cell(i, j));
        }
    }
    return faces;
}

float meanEdgeLength(const Mesh& mesh) {
    double sum = 0.0;
    int n = 0;
    for (Index i = 0; i < mesh.edgeCapacity(); ++i) {
        const EdgeId e{i};
        if (!mesh.isAlive(e)) {
            continue;
        }
        const auto [a, b] = mesh.edgeVertices(e);
        sum += static_cast<double>(length(mesh.position(a) - mesh.position(b)));
        ++n;
    }
    return static_cast<float>(sum / static_cast<double>(n));
}

bool bitwiseEqual(Vec3 a, Vec3 b) {
    return std::memcmp(&a.x, &b.x, sizeof(float)) == 0 &&
           std::memcmp(&a.y, &b.y, sizeof(float)) == 0 &&
           std::memcmp(&a.z, &b.z, sizeof(float)) == 0;
}

std::set<std::pair<Index, Index>> interfaceEdgeSet(const Mesh& mesh,
                                                   const remesh::RegionSolve& region) {
    std::set<std::pair<Index, Index>> edges;
    for (Index i = 0; i < mesh.edgeCapacity(); ++i) {
        const EdgeId e{i};
        if (!region.isInterfaceEdge(mesh, e)) {
            continue;
        }
        const auto [a, b] = mesh.edgeVertices(e);
        edges.insert({std::min(a.value, b.value), std::max(a.value, b.value)});
    }
    return edges;
}

// Runs a full region solve (isotropic + field-aligned quadrangulation) at
// `densityFactor` x the prescribed spacing, bypassing islands / merge /
// fillHoles / pureQuads by construction.
void solveRegion(Mesh& mesh, const remesh::RegionSolve& region, float densityFactor,
                 float baseEdge) {
    const float target = baseEdge / densityFactor;
    const remesh::ReferenceSurface reference(mesh, 0.0f);
    remesh::IsotropicOptions iso;
    iso.targetEdgeLength = target;
    iso.iterations = 3;
    iso.region = &region;
    REQUIRE(remesh::isotropicRemesh(mesh, reference, iso) == remesh::IsotropicStatus::Success);
    auto quad = remesh::makeFieldAlignedQuadrangulator();
    REQUIRE(quad->quadrangulate(mesh, target, nullptr, nullptr).success);
}

}  // namespace

TEST_CASE("region solve preserves prescribed positions bitwise (Invariant P)") {
    for (const float density : {1.0f, 4.0f}) {
        Mesh mesh = flatGrid();
        const float baseEdge = meanEdgeLength(mesh);
        auto built = remesh::buildRegionSolve(mesh, centreBlock(), 30.0f);
        REQUIRE(built.ok());

        std::map<Index, Vec3> before;
        for (Index i = 0; i < mesh.vertexCapacity(); ++i) {
            const VertexId v{i};
            if (mesh.isAlive(v) && built.region.pinned(v)) {
                before[i] = mesh.position(v);
            }
        }
        REQUIRE(before.size() > 0);

        solveRegion(mesh, built.region, density, baseEdge);

        for (const auto& [idx, pos] : before) {
            const VertexId v{idx};
            CAPTURE(idx);
            CAPTURE(density);
            REQUIRE(mesh.isAlive(v));
            CHECK(bitwiseEqual(mesh.position(v), pos));
        }
    }
}

TEST_CASE("region solve leaves frozen topology identical (Invariant F)") {
    Mesh mesh = domedGrid();
    const float baseEdge = meanEdgeLength(mesh);
    auto built = remesh::buildRegionSolve(mesh, centreBlock(), 30.0f);
    REQUIRE(built.ok());

    std::map<Index, std::vector<Index>> rings;
    for (Index i = 0; i < mesh.faceCapacity(); ++i) {
        const FaceId f{i};
        if (!mesh.isAlive(f) || !built.region.frozen(f)) {
            continue;
        }
        std::vector<Index> ring;
        for (const VertexId v : mesh.faceVertices(f)) {
            ring.push_back(v.value);
        }
        rings[i] = ring;
    }
    REQUIRE(rings.size() == 20);  // 36 grid quads minus the 16-face region

    solveRegion(mesh, built.region, 4.0f, baseEdge);

    for (const auto& [idx, ring] : rings) {
        const FaceId f{idx};
        CAPTURE(idx);
        REQUIRE(mesh.isAlive(f));
        std::vector<Index> now;
        for (const VertexId v : mesh.faceVertices(f)) {
            now.push_back(v.value);
        }
        CHECK(now == ring);  // same id, same ring, same order
    }
}

TEST_CASE("region solve preserves the interface edge set at 4x density") {
    // The SplitPass guard is what this pins. A low-density run passes with or
    // without it; only an over-fine target actually exercises the split path.
    // Without the guard this fixture loses 16 interface edges and corrupts 20
    // frozen rings, while every prescribed POSITION still matches — which is
    // exactly why a positional assertion is not sufficient evidence.
    Mesh mesh = flatGrid();
    const float baseEdge = meanEdgeLength(mesh);
    auto built = remesh::buildRegionSolve(mesh, centreBlock(), 30.0f);
    REQUIRE(built.ok());

    const auto before = interfaceEdgeSet(mesh, built.region);
    REQUIRE(before.size() == 16);  // perimeter of the 4x4 block

    solveRegion(mesh, built.region, 4.0f, baseEdge);

    CHECK(interfaceEdgeSet(mesh, built.region) == before);
}

TEST_CASE("the region interface is invisible to the plain boundary walk") {
    // Every interface edge has TWO incident faces, so retopo::boundaryChain —
    // which rests on edgeFaces(e).size() == 1 — cannot see it. That is why the
    // region-scoped walk exists.
    Mesh mesh = flatGrid();
    auto built = remesh::buildRegionSolve(mesh, centreBlock(), 30.0f);
    REQUIRE(built.ok());

    for (Index i = 0; i < mesh.edgeCapacity(); ++i) {
        const EdgeId e{i};
        if (built.region.isInterfaceEdge(mesh, e)) {
            CHECK_FALSE(cyber::retopo::isBoundaryEdge(mesh, e));
            CHECK(cyber::retopo::boundaryChain(mesh, e).vertices.empty());
        }
    }

    REQUIRE(built.region.interfaceLoops.size() == 1);
    CHECK(built.region.interfaceLoops.front().size() == 16);
    CHECK(built.region.targetValence.size() == 16);
}

TEST_CASE("the interface prescription matches the surrounding cage") {
    Mesh mesh = flatGrid();
    auto built = remesh::buildRegionSolve(mesh, centreBlock(), 30.0f);
    REQUIRE(built.ok());

    // 4 corners of the block see 3 frozen quads and must supply 1; the 12
    // mid-side vertices see 2 and must supply 2.
    int ones = 0, twos = 0;
    for (const auto& [idx, qin] : built.region.requiredInRegion) {
        CHECK(built.region.targetValence.at(idx) == 4);  // all interior to the grid
        if (qin == 1) {
            ++ones;
        } else if (qin == 2) {
            ++twos;
        }
    }
    CHECK(ones == 4);
    CHECK(twos == 12);
}

TEST_CASE("a caller valence override replaces the cage-derived prescription") {
    Mesh probe = flatGrid();
    auto base = remesh::buildRegionSolve(probe, centreBlock(), 30.0f);
    REQUIRE(base.ok());
    const Index someInterface = base.region.requiredInRegion.begin()->first;

    Mesh mesh = flatGrid();
    std::unordered_map<Index, int> overrides{{someInterface, 5}};
    auto built = remesh::buildRegionSolve(mesh, centreBlock(), 30.0f, &overrides);
    REQUIRE(built.ok());
    CHECK(built.region.targetValence.at(someInterface) == 5);
}

TEST_CASE("hostile and malformed regions are refused with a distinct reason") {
    const auto statusFor = [](Mesh& mesh, const std::vector<FaceId>& faces) {
        return remesh::buildRegionSolve(mesh, faces, 30.0f).status;
    };

    SUBCASE("empty region") {
        Mesh mesh = flatGrid();
        CHECK(statusFor(mesh, {}) == remesh::RegionSolveStatus::EmptyRegion);
    }
    SUBCASE("repeated face") {
        Mesh mesh = flatGrid();
        CHECK(statusFor(mesh, {cell(1, 1), cell(1, 1)}) == remesh::RegionSolveStatus::InvalidFace);
    }
    SUBCASE("out-of-range face") {
        Mesh mesh = flatGrid();
        CHECK(statusFor(mesh, {FaceId{9999}}) == remesh::RegionSolveStatus::InvalidFace);
    }
    SUBCASE("whole mesh is refused, not aliased to the whole-mesh path") {
        Mesh mesh = flatGrid();
        std::vector<FaceId> all;
        for (Index i = 0; i < mesh.faceCapacity(); ++i) {
            if (mesh.isAlive(FaceId{i})) {
                all.push_back(FaceId{i});
            }
        }
        CHECK(statusFor(mesh, all) == remesh::RegionSolveStatus::WholeMesh);
    }
    SUBCASE("disconnected region") {
        Mesh mesh = flatGrid();
        CHECK(statusFor(mesh, {cell(0, 0), cell(5, 5)}) ==
              remesh::RegionSolveStatus::Disconnected);
    }
    SUBCASE("coincident duplicates are refused, since weld cannot run here") {
        Mesh mesh = flatGrid();
        const Vec3 existing = mesh.position(VertexId{0});
        mesh.addVertex(existing);
        CHECK(statusFor(mesh, centreBlock()) == remesh::RegionSolveStatus::CoincidentVertices);
    }
}

TEST_CASE("a refused region leaves the mesh untouched") {
    Mesh mesh = flatGrid();
    const std::size_t faces = mesh.faceCount();
    std::vector<Index> ringBefore;
    for (const VertexId v : mesh.faceVertices(cell(1, 1))) {
        ringBefore.push_back(v.value);
    }

    const std::vector<FaceId> disconnected{cell(0, 0), cell(5, 5)};
    CHECK_FALSE(remesh::buildRegionSolve(mesh, disconnected, 30.0f).ok());

    CHECK(mesh.faceCount() == faces);  // no triangulation happened
    std::vector<Index> ringAfter;
    for (const VertexId v : mesh.faceVertices(cell(1, 1))) {
        ringAfter.push_back(v.value);
    }
    CHECK(ringAfter == ringBefore);
}

TEST_CASE("an empty region is the null object: whole-mesh behaviour is unchanged") {
    const remesh::RegionSolve none;
    CHECK(none.empty());
    // Every face reads active and every vertex unpinned, so a null region
    // cannot gate anything — this is what keeps the whole-mesh path byte-identical.
    CHECK(none.active(FaceId{0}));
    CHECK(none.active(FaceId{12345}));
    CHECK_FALSE(none.pinned(VertexId{0}));
}

// --- task 4: the pipeline region branch -------------------------------------

TEST_CASE("pipeline region solve rewrites only the region and keeps frozen ids") {
    Mesh mesh = flatGrid();
    const auto faces = centreBlock();

    std::map<Index, std::vector<Index>> frozenBefore;
    std::map<Index, Vec3> posBefore;
    {
        // Recompute the frozen set independently of the pipeline: every live
        // face not named, and every vertex on one.
        std::set<Index> named;
        for (const FaceId f : faces) {
            named.insert(f.value);
        }
        for (Index i = 0; i < mesh.faceCapacity(); ++i) {
            const FaceId f{i};
            if (!mesh.isAlive(f) || named.count(i) != 0) {
                continue;
            }
            std::vector<Index> ring;
            for (const VertexId v : mesh.faceVertices(f)) {
                ring.push_back(v.value);
                posBefore[v.value] = mesh.position(v);
            }
            frozenBefore[i] = ring;
        }
    }

    remesh::Parameters params;
    params.targetQuadCount = 400;
    const remesh::PipelineResult out = remesh::remesh(mesh, params, nullptr, nullptr, {}, {}, faces);

    REQUIRE(out.status == remesh::RunStatus::Success);
    CHECK(out.region.solvedFaces.size() > 0);
    CHECK(out.region.interfaceVertices.size() == 16);

    // Every frozen face survives under its own id, with its ring intact...
    for (const auto& [idx, ring] : frozenBefore) {
        const FaceId f{idx};
        CAPTURE(idx);
        REQUIRE(out.mesh.isAlive(f));
        std::vector<Index> now;
        for (const VertexId v : out.mesh.faceVertices(f)) {
            now.push_back(v.value);
        }
        CHECK(now == ring);
    }
    // ...and every one of its vertices is bitwise where it was.
    for (const auto& [idx, p] : posBefore) {
        const VertexId v{idx};
        CAPTURE(idx);
        REQUIRE(out.mesh.isAlive(v));
        CHECK(bitwiseEqual(out.mesh.position(v), p));
    }
    // The region itself really was rewritten, not merely preserved.
    CHECK(out.mesh.faceCount() != mesh.faceCount());
    CHECK(out.stats.targetEdgeLength > 0.0f);
}

TEST_CASE("an empty region runs the whole-mesh pipeline unchanged") {
    Mesh mesh = flatGrid();
    remesh::Parameters params;
    params.targetQuadCount = 400;

    const remesh::PipelineResult plain = remesh::remesh(mesh, params);
    const remesh::PipelineResult empty =
        remesh::remesh(mesh, params, nullptr, nullptr, {}, {}, {});

    REQUIRE(plain.status == empty.status);
    REQUIRE(plain.mesh.faceCount() == empty.mesh.faceCount());
    REQUIRE(plain.mesh.vertexCapacity() == empty.mesh.vertexCapacity());
    for (Index i = 0; i < plain.mesh.vertexCapacity(); ++i) {
        const VertexId v{i};
        if (plain.mesh.isAlive(v)) {
            REQUIRE(empty.mesh.isAlive(v));
            CHECK(bitwiseEqual(plain.mesh.position(v), empty.mesh.position(v)));
        }
    }
    CHECK(empty.region.empty());  // no region report on a whole-mesh run
}

TEST_CASE("region solve refuses parameters it cannot honour, rather than ignoring them") {
    Mesh mesh = flatGrid();
    const auto faces = centreBlock();

    SUBCASE("pureQuads is fatal, and caught by validation") {
        remesh::Parameters params;
        params.pureQuads = true;
        const auto out = remesh::remesh(mesh, params, nullptr, nullptr, {}, {}, faces);
        CHECK(out.status == remesh::RunStatus::Error);
        CHECK(std::any_of(out.parameterIssues.begin(), out.parameterIssues.end(),
                          [](const remesh::ParameterIssue& i) {
                              return i.parameter == "pureQuads" && i.fatal;
                          }));
    }
    SUBCASE("inapplicable defaults are skipped but REPORTED, not silently dropped") {
        // holeFillMaxBoundary (64) and smallPatchPolicy (KeepLargest) are the
        // DEFAULTS, so refusing on them would reject every ordinary call. Both
        // are meaningless for one connected region bounded by frozen topology.
        remesh::Parameters params;
        params.targetQuadCount = 400;
        const auto out = remesh::remesh(mesh, params, nullptr, nullptr, {}, {}, faces);
        REQUIRE(out.status == remesh::RunStatus::Success);
        const auto named = [&](const char* which) {
            return std::any_of(out.parameterIssues.begin(), out.parameterIssues.end(),
                               [&](const remesh::ParameterIssue& i) {
                                   return i.parameter == which && !i.fatal;
                               });
        };
        CHECK(named("holeFillMaxBoundary"));
        CHECK(named("smallPatchPolicy"));
    }
    SUBCASE("a refused region reports why") {
        remesh::Parameters params;
        const std::vector<FaceId> disconnected{cell(0, 0), cell(5, 5)};
        const auto out = remesh::remesh(mesh, params, nullptr, nullptr, {}, {}, disconnected);
        CHECK(out.status == remesh::RunStatus::Error);
        CHECK(out.error.find("face-connected") != std::string::npos);
    }
}

// --- task 7: the two-tier conformance gate ----------------------------------

TEST_CASE("interface irregularity is reported but never blocks the result") {
    // The spike measured 3 of 16 interface vertices irregular on this exact
    // fixture at prescribed density, and no local pass fixes it (it is a coupled
    // b-matching — design.md ADDENDUM 2). The contract is therefore: report it,
    // publish anyway. If this ever starts blocking, the rescope was undone.
    Mesh mesh = flatGrid();
    remesh::Parameters params;
    params.targetQuadCount = 400;
    const auto out = remesh::remesh(mesh, params, nullptr, nullptr, {}, {}, centreBlock());

    REQUIRE(out.status == remesh::RunStatus::Success);
    CHECK(out.mesh.faceCount() > 0);
    // Whatever the count is, every reported id must be a real interface vertex.
    const std::set<Index> iface = [&] {
        std::set<Index> s;
        for (const VertexId v : out.region.interfaceVertices) {
            s.insert(v.value);
        }
        return s;
    }();
    for (const VertexId v : out.region.interfaceIrregular) {
        CAPTURE(v.value);
        CHECK(iface.count(v.value) == 1);
    }
}

TEST_CASE("the refuse tier catches a broken prescription") {
    Mesh mesh = flatGrid();
    auto built = remesh::buildRegionSolve(mesh, centreBlock(), 30.0f);
    REQUIRE(built.ok());
    const remesh::InterfaceSnapshot before = remesh::captureInterface(mesh, built.region);

    SUBCASE("an untouched mesh is exact") {
        const auto r = remesh::verifyInterfaceConformance(mesh, built.region, before);
        CHECK(r.exact());
        CHECK(r.movedOrDeadPrescribed.empty());
        CHECK(r.corruptedFrozenFaces.empty());
        CHECK(r.lostInterfaceEdges.empty());
    }

    SUBCASE("a moved prescribed vertex is caught, and by bit pattern not epsilon") {
        const Index victim = before.prescribedPositions.begin()->first;
        const Vec3 p = mesh.position(VertexId{victim});
        // ONE ULP — the smallest change a float can represent. Every epsilon
        // comparison in existence passes this; the bitwise check must not. That
        // is exactly the failure mode it exists to prevent: "moved and snapped
        // back to within tolerance" is not "never touched".
        Mesh nudged = mesh;
        nudged.setPosition(VertexId{victim},
                           {std::nextafter(p.x, std::numeric_limits<float>::infinity()), p.y, p.z});
        const auto r = remesh::verifyInterfaceConformance(nudged, built.region, before);
        CHECK_FALSE(r.exact());
        REQUIRE(r.movedOrDeadPrescribed.size() == 1);
        CHECK(r.movedOrDeadPrescribed.front().value == victim);
        CHECK(r.describeFailure().find("prescribed vertices moved") != std::string::npos);
    }

    SUBCASE("a corrupted frozen ring is caught") {
        Mesh broken = mesh;
        const FaceId frozen{before.frozenRings.begin()->first};
        broken.removeFace(frozen);
        const auto r = remesh::verifyInterfaceConformance(broken, built.region, before);
        CHECK_FALSE(r.exact());
        CHECK(r.corruptedFrozenFaces.size() >= 1);
    }
}

// ---- External reference surface (openspec add-region-external-reference, 5.4b) ----
//
// A region solve builds its ReferenceSurface from the mesh it is REWRITING, so a fill
// refines its grown seed band and reprojects onto that approximation. Supplying the
// Target instead was measured at 0.42 quads mean / 1.26 quads MAX interior deviation
// better on rippled geometry; these lock the contract rather than the numbers.

namespace {

/// A dense rippled surface: detail far finer than a coarse cage's spacing, which is
/// the only geometry that can exhibit the defect at all. A smooth fixture cannot --
/// that is why 5.4b's originally filed 0.031-quad figure understated it tenfold.
Mesh rippledTarget(std::size_t n) {
    return makeGrid(n, [](float u, float v) {
        return 0.12f * std::sin(9.0f * u) * std::cos(9.0f * v);
    });
}
Mesh rippledCage() { return rippledTarget(kN); }

remesh::Parameters regionParams(float edge) {
    remesh::Parameters p;
    p.targetQuadCount = 400;
    p.edgeScale = 1.0f;
    p.sharpEdgeDegrees = 0.0f;
    p.smoothNormalDegrees = 0.0f;
    (void)edge;
    return p;
}

/// Mean/max distance of the SOLVED INTERIOR vertices from `truth`.
std::pair<double, double> interiorDeviation(const Mesh& solved, const remesh::RegionSolve& region,
                                           const remesh::ReferenceSurface& truth) {
    double mean = 0.0, worst = 0.0;
    std::size_t n = 0;
    for (Index i = 0; i < solved.vertexCapacity(); ++i) {
        const VertexId v{i};
        if (!solved.isAlive(v) || region.pinned(v)) {
            continue;  // interior only: interface vertices are never smoothed
        }
        const Vec3 p = solved.position(v);
        const double e = static_cast<double>(length(p - truth.project(p)));
        mean += e;
        worst = std::fmax(worst, e);
        ++n;
    }
    return {n ? mean / static_cast<double>(n) : 0.0, worst};
}

}  // namespace

TEST_CASE("an external reference pulls the region's interior onto the Target") {
    const Mesh target = rippledTarget(96);
    const remesh::ReferenceSurface targetRef(target, 0.0f);
    REQUIRE(!targetRef.empty());

    const Mesh cage = rippledCage();
    const auto faces = centreBlock();
    const float baseEdge = meanEdgeLength(cage);
    const remesh::Parameters params = regionParams(baseEdge);

    const remesh::PipelineResult self =
        remesh::remesh(cage, params, nullptr, nullptr, {}, {}, faces, nullptr, nullptr);
    const remesh::PipelineResult external =
        remesh::remesh(cage, params, nullptr, nullptr, {}, {}, faces, nullptr, &targetRef);
    REQUIRE(self.status == remesh::RunStatus::Success);
    REQUIRE(external.status == remesh::RunStatus::Success);

    // On a scratch copy for the same reason: buildRegionSolve mutates its argument.
    Mesh probe = cage;
    const auto builtSelf = remesh::buildRegionSolve(probe, faces, 0.0f, {});
    REQUIRE(builtSelf.status == remesh::RegionSolveStatus::Ok);
    const auto [selfMean, selfMax] = interiorDeviation(self.mesh, builtSelf.region, targetRef);
    const auto [extMean, extMax] = interiorDeviation(external.mesh, builtSelf.region, targetRef);

    // The property, not the measured constant: projecting onto the Target must put the
    // interior CLOSER to the Target than projecting onto the seed's approximation of it.
    CHECK(extMean < selfMean);
    CHECK(extMax < selfMax);
}

TEST_CASE("no external reference is byte-identical to before the parameter existed") {
    const Mesh cage = rippledCage();
    const auto faces = centreBlock();
    const remesh::Parameters params = regionParams(meanEdgeLength(cage));

    // Defaulted vs explicit nullptr: the capability must be inert when unused, or it
    // changes existing output by merely existing.
    const remesh::PipelineResult a =
        remesh::remesh(cage, params, nullptr, nullptr, {}, {}, faces, nullptr);
    const remesh::PipelineResult b =
        remesh::remesh(cage, params, nullptr, nullptr, {}, {}, faces, nullptr, nullptr);
    REQUIRE(a.status == remesh::RunStatus::Success);
    REQUIRE(b.status == remesh::RunStatus::Success);
    REQUIRE(a.mesh.faceCount() == b.mesh.faceCount());
    for (Index i = 0; i < a.mesh.vertexCapacity(); ++i) {
        const VertexId v{i};
        if (!a.mesh.isAlive(v)) {
            continue;
        }
        REQUIRE(b.mesh.isAlive(v));
        CHECK(bitwiseEqual(a.mesh.position(v), b.mesh.position(v)));
    }
}

TEST_CASE("an external reference does not weaken exact landing") {
    const Mesh target = rippledTarget(96);
    const remesh::ReferenceSurface targetRef(target, 0.0f);
    const Mesh cage = rippledCage();
    const auto faces = centreBlock();

    // buildRegionSolve takes Mesh& and TAGS interface edges as feature edges, so it
    // must run on a scratch copy: probing the mesh first and then solving it would
    // hand the pipeline an already-tagged input, which it refuses.
    std::map<Index, Vec3> before;
    {
        Mesh probe = cage;
        const auto built = remesh::buildRegionSolve(probe, faces, 0.0f, {});
        REQUIRE(built.status == remesh::RegionSolveStatus::Ok);
        for (const auto& entry : built.region.requiredInRegion) {
            const VertexId v{entry.first};
            before[v.value] = probe.position(v);
        }
    }

    const remesh::PipelineResult out = remesh::remesh(
        cage, regionParams(meanEdgeLength(cage)), nullptr, nullptr, {}, {}, faces, nullptr,
        &targetRef);
    REQUIRE(out.status == remesh::RunStatus::Success);

    // 5.3's guarantee is bitwise and must survive projecting the interior elsewhere.
    for (const auto& [id, position] : before) {
        const VertexId v{id};
        REQUIRE(out.mesh.isAlive(v));
        CHECK(bitwiseEqual(out.mesh.position(v), position));
    }
}

TEST_CASE("an unusable external reference is refused, not silently ignored") {
    const remesh::ReferenceSurface empty;  // default-constructed: nothing to project onto
    REQUIRE(empty.empty());
    const Mesh cage = rippledCage();
    const remesh::PipelineResult out =
        remesh::remesh(cage, regionParams(meanEdgeLength(cage)), nullptr, nullptr, {}, {},
                       centreBlock(), nullptr, &empty);
    // Falling back to the working mesh would make an unusable reference
    // indistinguishable from a working one from outside the engine.
    CHECK(out.status == remesh::RunStatus::Error);
}

TEST_CASE("a whole-mesh solve ignores an external reference") {
    const Mesh target = rippledTarget(96);
    const remesh::ReferenceSurface targetRef(target, 0.0f);
    const Mesh input = rippledCage();
    const remesh::Parameters params = regionParams(meanEdgeLength(input));

    const remesh::PipelineResult plain = remesh::remesh(input, params);
    const remesh::PipelineResult withRef =
        remesh::remesh(input, params, nullptr, nullptr, {}, {}, {}, nullptr, &targetRef);
    REQUIRE(plain.status == remesh::RunStatus::Success);
    REQUIRE(withRef.status == remesh::RunStatus::Success);
    CHECK(plain.mesh.faceCount() == withRef.mesh.faceCount());
}

// ---- Authored density brush (openspec add-weave-density-radial-symmetry, 5.2b) ----
//
// Per-vertex multipliers on the target edge length, MULTIPLIED into the
// curvature-derived scales and clamped to the same [0.3, 3.0] band. The composition
// rule was decided before the code (IsotropicOptions::densityScales) precisely so
// these tests assert a stated rule rather than whatever the implementation happened
// to do.

TEST_CASE("an authored density scale changes the solve, and absent is byte-identical") {
    const Mesh cage = rippledCage();
    const auto faces = centreBlock();
    const remesh::Parameters params = regionParams(meanEdgeLength(cage));

    const remesh::PipelineResult plain =
        remesh::remesh(cage, params, nullptr, nullptr, {}, {}, faces, nullptr, nullptr, nullptr);
    REQUIRE(plain.status == remesh::RunStatus::Success);

    // Uniformly FINER: a 0.5 multiplier everywhere must produce more faces.
    std::vector<float> finer(cage.vertexCapacity(), 0.5f);
    const remesh::PipelineResult fine =
        remesh::remesh(cage, params, nullptr, nullptr, {}, {}, faces, nullptr, nullptr, &finer);
    REQUIRE(fine.status == remesh::RunStatus::Success);
    CHECK(fine.mesh.faceCount() > plain.mesh.faceCount());

    // Uniformly COARSER must go the other way, or the sign convention is inverted --
    // which a single-direction test would not catch.
    std::vector<float> coarser(cage.vertexCapacity(), 2.0f);
    const remesh::PipelineResult coarse =
        remesh::remesh(cage, params, nullptr, nullptr, {}, {}, faces, nullptr, nullptr, &coarser);
    REQUIRE(coarse.status == remesh::RunStatus::Success);
    CHECK(coarse.mesh.faceCount() < plain.mesh.faceCount());

    // All-1.0 is the identity: the brush must be inert when it says "no change".
    std::vector<float> neutral(cage.vertexCapacity(), 1.0f);
    const remesh::PipelineResult identity =
        remesh::remesh(cage, params, nullptr, nullptr, {}, {}, faces, nullptr, nullptr, &neutral);
    REQUIRE(identity.status == remesh::RunStatus::Success);
    CHECK(identity.mesh.faceCount() == plain.mesh.faceCount());
}

TEST_CASE("density composition is clamped to the curvature band, not beyond it") {
    // The stated rule: multiply, then clamp to [0.3, 3.0]. An authored 10x must
    // therefore behave exactly like an authored 3x -- if the product escaped the band
    // the split/collapse thresholds would be running outside their tuned range, which
    // is the compounding runaway documented at kScaleAttribute.
    const Mesh cage = rippledCage();
    const auto faces = centreBlock();
    remesh::Parameters params = regionParams(meanEdgeLength(cage));
    params.adaptivity = 0.0f;  // isolate the authored term from curvature

    std::vector<float> atBand(cage.vertexCapacity(), 3.0f);
    std::vector<float> pastBand(cage.vertexCapacity(), 10.0f);
    const remesh::PipelineResult a =
        remesh::remesh(cage, params, nullptr, nullptr, {}, {}, faces, nullptr, nullptr, &atBand);
    const remesh::PipelineResult b =
        remesh::remesh(cage, params, nullptr, nullptr, {}, {}, faces, nullptr, nullptr, &pastBand);
    REQUIRE(a.status == remesh::RunStatus::Success);
    REQUIRE(b.status == remesh::RunStatus::Success);
    CHECK(a.mesh.faceCount() == b.mesh.faceCount());
}

TEST_CASE("a short density array is honoured where it applies and defaults elsewhere") {
    // Callers should not have to size the array to the full vertex capacity; entries
    // past the end read as 1.0 rather than 0 (which would collapse the target length).
    const Mesh cage = rippledCage();
    const auto faces = centreBlock();
    const remesh::Parameters params = regionParams(meanEdgeLength(cage));
    std::vector<float> shortArray(4, 0.5f);
    const remesh::PipelineResult out = remesh::remesh(cage, params, nullptr, nullptr, {}, {},
                                                     faces, nullptr, nullptr, &shortArray);
    CHECK(out.status == remesh::RunStatus::Success);
    CHECK(out.mesh.faceCount() > 0);
}
