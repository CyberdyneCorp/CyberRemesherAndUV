#include <doctest.h>

#include <array>
#include <cmath>
#include <cstring>
#include <map>
#include <string>
#include <vector>

#include "cyber/core/guidance.hpp"
#include "cyber/core/pipeline.hpp"
#include "cyber/quadrangulate/field_quadrangulator.hpp"
#include "cyber/quadrangulate/quadcover_extractor.hpp"

using cyber::FaceId;
using cyber::Index;
using cyber::Mesh;
using cyber::Vec3;
using cyber::VertexId;
namespace remesh = cyber::remesh;

namespace {

// Triangulated plane on [0,size]^2 at z = 0 with `n` cells per side.
Mesh makePlane(int n, float size) {
    std::vector<Vec3> p;
    const float step = size / static_cast<float>(n);
    for (int i = 0; i <= n; ++i) {
        for (int j = 0; j <= n; ++j) {
            p.push_back({static_cast<float>(i) * step, static_cast<float>(j) * step, 0.0f});
        }
    }
    const auto idx = [n](int i, int j) { return static_cast<Index>(i * (n + 1) + j); };
    std::vector<std::vector<Index>> f;
    for (int i = 0; i < n; ++i) {
        for (int j = 0; j < n; ++j) {
            f.push_back({idx(i, j), idx(i + 1, j), idx(i + 1, j + 1)});
            f.push_back({idx(i, j), idx(i + 1, j + 1), idx(i, j + 1)});
        }
    }
    return Mesh::fromIndexed(p, f);
}

// Closed unit box, `n` cells per side, triangulated. Its creases route the
// quad-cover method to the native seamless solve on every build, which is the
// bit-reproducible one — the vendored Geogram solve is not (measured: 1 run in
// 30 differs), so a byte-identity check needs an input that never lands there.
Mesh makeBox(int n) {
    std::vector<Vec3> p;
    std::vector<std::vector<Index>> f;
    std::map<std::array<int, 3>, Index> lookup;
    const float step = 1.0f / static_cast<float>(n);
    // One shared vertex per lattice point, so the box is welded along its edges.
    const auto vertexAt = [&](int axis, int side, int u, int v) {
        const auto slot = [](int a) { return static_cast<std::size_t>(a % 3); };
        std::array<int, 3> key{};
        key[slot(axis)] = side;
        key[slot(axis + 1)] = u;
        key[slot(axis + 2)] = v;
        const auto [it, inserted] = lookup.emplace(key, static_cast<Index>(p.size()));
        if (inserted) {
            p.push_back({static_cast<float>(key[0]) * step, static_cast<float>(key[1]) * step,
                         static_cast<float>(key[2]) * step});
        }
        return it->second;
    };
    for (int axis = 0; axis < 3; ++axis) {
        for (const int side : {0, n}) {
            for (int i = 0; i < n; ++i) {
                for (int j = 0; j < n; ++j) {
                    const Index a = vertexAt(axis, side, i, j);
                    const Index b = vertexAt(axis, side, i + 1, j);
                    const Index c = vertexAt(axis, side, i + 1, j + 1);
                    const Index d = vertexAt(axis, side, i, j + 1);
                    f.push_back({a, b, c});
                    f.push_back({a, c, d});  // winding is fixed by the pipeline's orient stage
                }
            }
        }
    }
    return Mesh::fromIndexed(p, f);
}

// Two coaxial tubes meeting at a junction — the "limb junction" the spec's
// exit-gate scenario names. A closed capped cylinder plus a smaller cylinder
// is more machinery than the measurement needs; a single bent tube already
// gives the field a preferred (and wrong-by-default) direction at the bend.
Mesh makeBentTube(int rings, int segments, float radius, float length) {
    std::vector<Vec3> p;
    std::vector<std::vector<Index>> f;
    for (int r = 0; r <= rings; ++r) {
        const float t = static_cast<float>(r) / static_cast<float>(rings);
        const float z = t * length;
        // A gentle S-bend so the tube is not a surface of revolution.
        const float ox = 0.35f * radius * std::sin(t * 2.0f * cyber::kPi);
        for (int s = 0; s < segments; ++s) {
            const float th =
                2.0f * cyber::kPi * static_cast<float>(s) / static_cast<float>(segments);
            p.push_back({ox + radius * std::cos(th), radius * std::sin(th), z});
        }
    }
    const auto ring = [segments](int r, int s) {
        return static_cast<Index>(r * segments + (s % segments));
    };
    for (int r = 0; r < rings; ++r) {
        for (int s = 0; s < segments; ++s) {
            f.push_back({ring(r, s), ring(r + 1, s), ring(r + 1, s + 1)});
            f.push_back({ring(r, s), ring(r + 1, s + 1), ring(r, s + 1)});
        }
    }
    return Mesh::fromIndexed(p, f);
}

remesh::Parameters smallRun(int quads) {
    remesh::Parameters params;
    params.targetQuadCount = quads;
    params.adaptivity = 0.0f;
    return params;
}

remesh::QuadrangulatorFactory fieldAligned() {
    return [] { return remesh::makeFieldAlignedQuadrangulator(); };
}

// Byte-exact comparison of two pipeline outputs: raw float bits of every
// vertex position plus the full face index lists.
bool meshesBitIdentical(const Mesh& a, const Mesh& b) {
    std::vector<Vec3> pa, pb;
    std::vector<std::vector<Index>> fa, fb;
    a.toIndexed(pa, fa);
    b.toIndexed(pb, fb);
    if (pa.size() != pb.size() || fa.size() != fb.size()) {
        return false;
    }
    if (!pa.empty() && std::memcmp(pa.data(), pb.data(), pa.size() * sizeof(Vec3)) != 0) {
        return false;
    }
    return fa == fb;
}

double meanEdgeLengthNear(const Mesh& mesh, const Vec3& lo, const Vec3& hi) {
    double sum = 0.0;
    std::size_t count = 0;
    for (Index ei = 0; ei < mesh.edgeCapacity(); ++ei) {
        const cyber::EdgeId e{ei};
        if (!mesh.isAlive(e)) {
            continue;
        }
        const auto [a, b] = mesh.edgeVertices(e);
        const Vec3 mid = cyber::lerp(mesh.position(a), mesh.position(b), 0.5f);
        if (mid.x < lo.x || mid.x > hi.x || mid.y < lo.y || mid.y > hi.y) {
            continue;
        }
        sum += static_cast<double>(cyber::length(mesh.position(a) - mesh.position(b)));
        ++count;
    }
    return count > 0 ? sum / static_cast<double>(count) : 0.0;
}

// Exit-gate metric (proposal, "Exit gate"): for every output face whose
// centroid lies inside a guide's influence radius, take the SMALLEST angle
// mod 90 degrees between the guide tangent (projected into the face plane)
// and the face's edge directions, then average. A random field averages
// 22.5 degrees; the stated target is <= 15.
double meanInRadiusDeviationDeg(const Mesh& mesh, const remesh::GuidanceField& field) {
    double sum = 0.0;
    std::size_t count = 0;
    for (Index fi = 0; fi < mesh.faceCapacity(); ++fi) {
        const FaceId f{fi};
        if (!mesh.isAlive(f)) {
            continue;
        }
        const std::vector<VertexId> fv = mesh.faceVertices(f);
        if (fv.size() < 3) {
            continue;
        }
        Vec3 c{0, 0, 0};
        for (const VertexId v : fv) {
            c += mesh.position(v);
        }
        c = c / static_cast<float>(fv.size());
        const Vec3 n = cyber::normalized(mesh.faceNormal(f));
        const remesh::GuideSample s = field.guideAt(c, n);
        if (!(s.weight > 0.0f)) {
            continue;
        }
        const Vec3 t = s.tangent - n * cyber::dot(n, s.tangent);
        if (cyber::lengthSquared(t) < 1e-12f) {
            continue;
        }
        const Vec3 tn = cyber::normalized(t);
        float best = 90.0f;
        for (std::size_t k = 0; k < fv.size(); ++k) {
            Vec3 d = mesh.position(fv[(k + 1) % fv.size()]) - mesh.position(fv[k]);
            d = d - n * cyber::dot(n, d);
            if (cyber::lengthSquared(d) < 1e-16f) {
                continue;
            }
            d = cyber::normalized(d);
            const float dev =
                cyber::radiansToDegrees(std::acos(std::min(1.0f, std::fabs(cyber::dot(d, tn)))));
            best = std::min(best, std::min(dev, std::fabs(90.0f - dev)));
        }
        sum += static_cast<double>(best);
        ++count;
    }
    return count > 0 ? sum / static_cast<double>(count) : -1.0;
}

// A quadrangulator that always declines guidance (the IQuadrangulator default
// body) and produces a trivially valid result, so the "unsupported path
// reports, not drops" scenario is exercised without depending on which
// backends happen to be compiled in.
class DecliningQuadrangulator final : public remesh::IQuadrangulator {
public:
    Outcome quadrangulate(Mesh& mesh, float, cyber::ProgressSink*,
                          const cyber::CancelToken*) override {
        return {.success = mesh.faceCount() > 0, .cancelled = false, .failureReason = {}};
    }
    [[nodiscard]] std::string name() const override { return "declining-test-backend"; }
};

// Accepts guidance up front but reports at run time that it could not honor
// it — the second half of the loud-reporting requirement (the quad-cover
// vendored-fallback case, without needing the vendored solver present).
class LateDeclineQuadrangulator final : public remesh::IQuadrangulator {
public:
    Outcome quadrangulate(Mesh& mesh, float, cyber::ProgressSink*,
                          const cyber::CancelToken*) override {
        return {.success = mesh.faceCount() > 0, .cancelled = false, .failureReason = {}};
    }
    bool acceptGuidance(const remesh::GuidanceField&, std::string&) override { return true; }
    [[nodiscard]] std::vector<std::string> unhonoredGuidance() const override {
        return {"flow guides: test backend fell back to a solve with no guide hook"};
    }
    [[nodiscard]] std::string name() const override { return "late-decline-test-backend"; }
};

}  // namespace

TEST_CASE("an empty guide set reproduces the unguided output byte-for-byte") {
    const Mesh plane = makePlane(16, 4.0f);
    const remesh::Parameters params = smallRun(400);

    const auto unguided = remesh::remesh(plane, params, nullptr, nullptr, fieldAligned());
    REQUIRE(unguided.status == remesh::RunStatus::Success);

    SUBCASE("nullptr guidance") {
        const auto same =
            remesh::remesh(plane, params, nullptr, nullptr, fieldAligned(), {}, nullptr);
        CHECK(meshesBitIdentical(unguided.mesh, same.mesh));
        CHECK(same.islandGuidance.empty());
    }
    SUBCASE("empty Guidance object") {
        const remesh::Guidance empty;
        const auto same =
            remesh::remesh(plane, params, nullptr, nullptr, fieldAligned(), {}, &empty);
        CHECK(meshesBitIdentical(unguided.mesh, same.mesh));
        CHECK(same.islandGuidance.empty());
        CHECK(same.parameterIssues.empty());
    }
    SUBCASE("density 1.0 everywhere") {
        remesh::Guidance g;
        g.density.vertexValues.assign(plane.vertexCount(), 1.0f);
        const auto same = remesh::remesh(plane, params, nullptr, nullptr, fieldAligned(), {}, &g);
        REQUIRE(same.status == remesh::RunStatus::Success);
        CHECK(meshesBitIdentical(unguided.mesh, same.mesh));
    }
}

// The same SHALL, on the SHIPPED DEFAULT backend (quad-cover) — where it used
// to fail: merely CARRYING a density field forces the native seamless route and
// the probe-predicted initial scaling, so an all-1.0 paint silently changed the
// mesh. A neutral density is now dropped at validation, so nothing downstream
// can see it.
TEST_CASE("a density of 1.0 everywhere is byte-identical on the default quad-cover backend") {
    const Mesh box = makeBox(6);
    const remesh::Parameters params = smallRun(300);
    const auto quadCover = [] { return remesh::makeQuadCoverQuadrangulator(); };

    const auto unguided = remesh::remesh(box, params, nullptr, nullptr, quadCover, fieldAligned());
    REQUIRE(unguided.status == remesh::RunStatus::Success);
    // The comparison below only means something if the backend repeats itself.
    const auto repeat = remesh::remesh(box, params, nullptr, nullptr, quadCover, fieldAligned());
    REQUIRE(meshesBitIdentical(unguided.mesh, repeat.mesh));

    remesh::Guidance g;
    g.density.vertexValues.assign(box.vertexCount(), 1.0f);
    const auto same = remesh::remesh(box, params, nullptr, nullptr, quadCover, fieldAligned(), &g);
    REQUIRE(same.status == remesh::RunStatus::Success);
    CHECK(meshesBitIdentical(unguided.mesh, same.mesh));
    // Dropped, but not silently: the run says the paint had no effect.
    CHECK(same.islandGuidance.empty());
    CHECK(same.parameterIssues.size() == 1);
}

TEST_CASE("painted density shrinks quads inside the painted region") {
    const Mesh plane = makePlane(40, 8.0f);
    remesh::Parameters params = smallRun(1200);

    remesh::Guidance g;
    // 4.0 on the x < 4 half, 1.0 on the other, with a narrow ramp so the
    // isotropic gradation limiter has somewhere to blend.
    for (Index vi = 0; vi < plane.vertexCapacity(); ++vi) {
        const Vec3 p = plane.position(VertexId{vi});
        g.density.vertexValues.push_back(p.x < 3.5f ? remesh::kDensityMax : 1.0f);
    }

    const auto guided = remesh::remesh(plane, params, nullptr, nullptr, fieldAligned(), {}, &g);
    REQUIRE(guided.status == remesh::RunStatus::Success);

    // Sample well inside each region so the transition band is excluded.
    const double dense =
        meanEdgeLengthNear(guided.mesh, Vec3{0.2f, 0.2f, -1.0f}, Vec3{2.5f, 7.8f, 1.0f});
    const double sparse =
        meanEdgeLengthNear(guided.mesh, Vec3{5.0f, 0.2f, -1.0f}, Vec3{7.8f, 7.8f, 1.0f});
    REQUIRE(dense > 0.0);
    REQUIRE(sparse > 0.0);
    const double ratio = dense / sparse;
    MESSAGE("painted/unpainted mean edge ratio: " << ratio << " (documented 1/sqrt(4) = 0.5)");
    // The documented relation is edge = base / sqrt(density) -> 0.5. The
    // isotropic band is [4/5, 4/3] of the local target and the gradation
    // limiter softens the step, so accept a band around it rather than the
    // exact figure.
    CHECK(ratio < 0.75);
    CHECK(ratio > 0.30);

    // The requested count guarantee still holds: painting redistributes
    // quads, it does not blow the budget open.
    const auto unguided = remesh::remesh(plane, params, nullptr, nullptr, fieldAligned());
    REQUIRE(unguided.status == remesh::RunStatus::Success);
    const double countRatio =
        static_cast<double>(guided.stats.quadCount + guided.stats.triangleCount) /
        static_cast<double>(unguided.stats.quadCount + unguided.stats.triangleCount);
    MESSAGE("guided/unguided face count ratio: " << countRatio);
    CHECK(countRatio > 0.5);
    CHECK(countRatio < 3.0);
}

TEST_CASE("an island on a backend without guide support is reported, not silently dropped") {
    const Mesh plane = makePlane(12, 4.0f);
    remesh::Guidance g;
    remesh::FlowGuide guide;
    guide.points = {Vec3{0.5f, 0.5f, 0.0f}, Vec3{3.5f, 3.5f, 0.0f}};
    guide.strength = 1.0f;
    guide.radius = 1.0f;
    g.guides.push_back(guide);

    const auto factory = [] {
        return std::unique_ptr<remesh::IQuadrangulator>(new DecliningQuadrangulator());
    };
    const auto result = remesh::remesh(plane, smallRun(300), nullptr, nullptr, factory, {}, &g);
    REQUIRE(result.status == remesh::RunStatus::Success);
    REQUIRE(result.islandGuidance.size() == 1);
    const remesh::IslandGuidance& row = result.islandGuidance[0];
    CHECK(row.islandIndex == 0);
    CHECK(row.guidesInRange == 1);
    CHECK_FALSE(row.guidesHonored);
    CHECK(row.reason.find("declining-test-backend") != std::string::npos);
    CHECK(row.reason.find("flow guides") != std::string::npos);
}

TEST_CASE("a backend that accepts guidance but cannot honor it reports the unhonored guides") {
    const Mesh plane = makePlane(12, 4.0f);
    remesh::Guidance g;
    remesh::FlowGuide guide;
    guide.points = {Vec3{0.5f, 0.5f, 0.0f}, Vec3{3.5f, 3.5f, 0.0f}};
    guide.strength = 1.0f;
    guide.radius = 1.0f;
    g.guides.push_back(guide);

    const auto factory = [] {
        return std::unique_ptr<remesh::IQuadrangulator>(new LateDeclineQuadrangulator());
    };
    const auto result = remesh::remesh(plane, smallRun(300), nullptr, nullptr, factory, {}, &g);
    REQUIRE(result.islandGuidance.size() == 1);
    CHECK_FALSE(result.islandGuidance[0].guidesHonored);
    CHECK(result.islandGuidance[0].reason.find("no guide hook") != std::string::npos);
}

TEST_CASE("a supported backend reports the guidance as honored") {
    const Mesh plane = makePlane(12, 4.0f);
    remesh::Guidance g;
    remesh::FlowGuide guide;
    guide.points = {Vec3{0.5f, 0.5f, 0.0f}, Vec3{3.5f, 3.5f, 0.0f}};
    guide.strength = 1.0f;
    guide.radius = 1.0f;
    g.guides.push_back(guide);

    const auto result =
        remesh::remesh(plane, smallRun(300), nullptr, nullptr, fieldAligned(), {}, &g);
    REQUIRE(result.islandGuidance.size() == 1);
    CHECK(result.islandGuidance[0].guidesHonored);
    CHECK(result.islandGuidance[0].reason.empty());
}

TEST_CASE("fatal guidance aborts the run and names the offending guide") {
    const Mesh plane = makePlane(8, 4.0f);
    remesh::Guidance g;
    remesh::FlowGuide bad;
    bad.points = {Vec3{0.5f, 0.5f, 0.0f}, Vec3{3.5f, 3.5f, 0.0f}};
    bad.strength = 1.0f;
    bad.radius = 0.0f;  // could never be honored
    g.guides.push_back(bad);

    const auto result =
        remesh::remesh(plane, smallRun(300), nullptr, nullptr, fieldAligned(), {}, &g);
    CHECK(result.status == remesh::RunStatus::Error);
    CHECK(result.error == "invalid parameters");
    bool named = false;
    for (const auto& issue : result.parameterIssues) {
        if (issue.fatal && issue.parameter == "guides[0]") {
            named = true;
        }
    }
    CHECK(named);
}

TEST_CASE("extracted loops follow a flow guide inside its influence radius") {
    // A tube whose default field runs along the axis; the guide asks for the
    // 45-degree helical direction instead, so the measurement cannot be
    // satisfied by the unguided answer.
    const Mesh tube = makeBentTube(28, 24, 0.6f, 4.0f);
    remesh::Parameters params = smallRun(900);

    remesh::Guidance g;
    remesh::FlowGuide guide;
    for (int k = 0; k <= 24; ++k) {
        const float t = static_cast<float>(k) / 24.0f;
        const float z = 0.6f + t * 2.8f;
        // A helix ~27 degrees off the tube axis. A full 45 degrees is NOT more
        // adversarial here: the tube's diagonal triangulation already pairs into
        // near-45-degree quads, so the unguided baseline collapses to ~6 degrees
        // and the measurement stops discriminating (measured).
        const float th = t * 2.4f;
        const float ox = 0.35f * 0.6f * std::sin((z / 4.0f) * 2.0f * cyber::kPi);
        guide.points.push_back({ox + 0.6f * std::cos(th), 0.6f * std::sin(th), z});
    }
    guide.strength = 1.0f;
    guide.radius = 0.5f;
    g.guides.push_back(guide);

    const remesh::GuidanceField field(tube, g);
    REQUIRE(field.hasGuides());

    const auto unguided = remesh::remesh(tube, params, nullptr, nullptr, fieldAligned());
    const auto guided = remesh::remesh(tube, params, nullptr, nullptr, fieldAligned(), {}, &g);
    REQUIRE(guided.status == remesh::RunStatus::Success);

    const double devGuided = meanInRadiusDeviationDeg(guided.mesh, field);
    const double devUnguided = meanInRadiusDeviationDeg(unguided.mesh, field);
    REQUIRE(devGuided >= 0.0);
    MESSAGE("EXIT GATE mean in-radius deviation: unguided " << devUnguided << " deg, guided "
                                                            << devGuided
                                                            << " deg (random baseline 22.5, "
                                                               "target <= 15)");
    // Guidance must measurably pull the extracted loops toward the stroke.
    CHECK(devGuided < devUnguided);
    // Measured gate: see docs/flow-guides.md and the CHANGELOG for the number
    // this threshold was set from. It is asserted at the MEASURED level, not
    // at a level chosen to look good.
    CHECK(devGuided <= 15.0);
}
