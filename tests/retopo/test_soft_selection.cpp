#include <doctest.h>

#include <cmath>
#include <cstddef>
#include <limits>
#include <vector>

#include "cyber/core/mesh.hpp"
#include "cyber/retopo/build_tools.hpp"
#include "cyber/retopo/pins.hpp"
#include "cyber/retopo/snapping.hpp"
#include "cyber/retopo/soft_selection.hpp"

using cyber::Index;
using cyber::Mesh;
using cyber::Vec3;
using cyber::VertexId;
namespace retopo = cyber::retopo;

namespace {

constexpr float kPi = 3.14159265358979323846f;

// A cols x rows quad grid in the plane z = `z`, spanning x in [0, cols-1] and
// y in [0, rows-1]. Vertex index = row * cols + col, so column identity is easy
// to assert on.
Mesh makeGrid(int cols, int rows, float z = 0.0f) {
    std::vector<Vec3> p;
    p.reserve(static_cast<std::size_t>(cols * rows));
    for (int j = 0; j < rows; ++j) {
        for (int i = 0; i < cols; ++i) {
            p.push_back(Vec3{static_cast<float>(i), static_cast<float>(j), z});
        }
    }
    const Index stride = static_cast<Index>(cols);
    std::vector<std::vector<Index>> f;
    for (int j = 0; j + 1 < rows; ++j) {
        for (int i = 0; i + 1 < cols; ++i) {
            const Index a = static_cast<Index>(j * cols + i);
            f.push_back({a, a + 1, a + stride + 1, a + stride});
        }
    }
    return Mesh::fromIndexed(p, f);
}

// Radius of the truncated cone Target at height y: 1.0 at y = -0.5 tapering to
// 0.2 at y = 3.0. The EditMesh only covers y in [0, 2], so re-snapping never
// clamps against an end rim.
float coneRadius(float y) { return 1.0f + (0.2f - 1.0f) * (y + 0.5f) / 3.5f; }

// Open tube of quads around the y axis following `coneRadius`.
Mesh makeCone(int segments, int rings, float y0, float y1) {
    std::vector<Vec3> p;
    for (int r = 0; r <= rings; ++r) {
        const float y = y0 + (y1 - y0) * static_cast<float>(r) / static_cast<float>(rings);
        const float radius = coneRadius(y);
        for (int s = 0; s < segments; ++s) {
            const float a = 2.0f * kPi * static_cast<float>(s) / static_cast<float>(segments);
            p.push_back(Vec3{radius * std::cos(a), y, radius * std::sin(a)});
        }
    }
    const Index stride = static_cast<Index>(segments);
    std::vector<std::vector<Index>> f;
    for (int r = 0; r < rings; ++r) {
        for (int s = 0; s < segments; ++s) {
            const Index a = static_cast<Index>(r * segments + s);
            const Index b = static_cast<Index>(r * segments + (s + 1) % segments);
            f.push_back({a, b, b + stride, a + stride});
        }
    }
    return Mesh::fromIndexed(p, f);
}

std::vector<Vec3> snapshot(const Mesh& mesh) {
    std::vector<Vec3> out(static_cast<std::size_t>(mesh.vertexCapacity()));
    for (Index i = 0; i < mesh.vertexCapacity(); ++i) {
        out[i] = mesh.position(VertexId{i});
    }
    return out;
}

std::size_t countStrictlyFractional(const retopo::SoftSelection& selection) {
    std::size_t n = 0;
    for (const float w : selection.weights()) {
        if (w > 0.0f && w < 1.0f) {
            ++n;
        }
    }
    return n;
}

bool weightsInUnitRange(const retopo::SoftSelection& selection) {
    for (const float w : selection.weights()) {
        if (!(w >= 0.0f && w <= 1.0f)) {
            return false;
        }
    }
    return true;
}

float radialDistance(Vec3 p) { return std::sqrt(p.x * p.x + p.z * p.z); }

}  // namespace

TEST_CASE("a non-finite weight cannot enter the field or move a vertex") {
    // Regression: the [0,1] invariant was enforced with std::clamp alone, and
    // std::clamp returns NaN unchanged (both of its comparisons are false for
    // NaN). A NaN weight then slipped past the `w <= 0` early-out in
    // transformWeighted and lerped the vertex to (nan,nan,nan) — one bad weight
    // from a binding or a file silently destroyed the mesh.
    const float nan = std::numeric_limits<float>::quiet_NaN();
    const float inf = std::numeric_limits<float>::infinity();

    retopo::SoftSelection selection;
    selection.setWeight(VertexId{0}, nan);
    selection.setWeight(VertexId{1}, inf);
    selection.setWeight(VertexId{2}, -inf);
    // Every non-finite value reads as "not selected". Saturating +inf to 1
    // would be defensible arithmetically, but an infinite weight is always an
    // upstream bug, and fully moving a vertex on a garbage value is worse than
    // leaving it alone.
    CHECK(selection.weight(VertexId{0}) == 0.0f);
    CHECK(selection.weight(VertexId{1}) == 0.0f);
    CHECK(selection.weight(VertexId{2}) == 0.0f);

    const std::vector<float> raw = {nan, 0.5f, inf};
    retopo::SoftSelection fromRaw;
    fromRaw.fromWeights(raw);
    for (std::size_t i = 0; i < raw.size(); ++i) {
        const float w = fromRaw.weight(VertexId{static_cast<Index>(i)});
        CHECK(std::isfinite(w));
        CHECK(w >= 0.0f);
        CHECK(w <= 1.0f);
    }

    // End to end: a NaN weight must leave its vertex exactly where it was.
    Mesh mesh = makeGrid(3, 3);
    retopo::SoftSelection sel;
    sel.setWeight(VertexId{0}, nan);
    const Vec3 before = mesh.position(VertexId{0});
    retopo::Affine move;
    move.translation = Vec3{100.0f, 100.0f, 100.0f};
    retopo::transformWeighted(mesh, sel, move, nullptr, nullptr);
    const Vec3 after = mesh.position(VertexId{0});
    CHECK(std::isfinite(after.x));
    CHECK(after.x == doctest::Approx(before.x));
    CHECK(after.y == doctest::Approx(before.y));
    CHECK(after.z == doctest::Approx(before.z));
}

// ---- gradient region selection: line ---------------------------------------

TEST_CASE("line gradient ramps 0 to 1 and saturates beyond the end") {
    const Mesh mesh = makeGrid(7, 2);  // x = 0..6
    const retopo::Falloff curves[] = {retopo::Falloff::Linear, retopo::Falloff::Smooth,
                                      retopo::Falloff::Sharp, retopo::Falloff::Round};

    for (const retopo::Falloff falloff : curves) {
        retopo::SoftSelection selection;
        retopo::LineRegion region;
        region.anchor = {1.0f, 0.0f, 0.0f};  // column 1 is the anchor A
        region.end = {5.0f, 0.0f, 0.0f};     // column 5 is the end B
        region.falloff = falloff;
        retopo::selectLine(mesh, selection, region);

        // At A exactly 0, at B exactly 1, saturated beyond B, zero behind A.
        CHECK(selection.weight(VertexId{1}) == 0.0f);
        CHECK(selection.weight(VertexId{0}) == 0.0f);
        CHECK(selection.weight(VertexId{5}) == 1.0f);
        CHECK(selection.weight(VertexId{6}) == 1.0f);

        // Strictly monotone across the ramp, and the midpoint follows the curve.
        for (Index i = 1; i < 5; ++i) {
            CHECK(selection.weight(VertexId{i}) < selection.weight(VertexId{i + 1}));
        }
        CHECK(selection.weight(VertexId{3}) == doctest::Approx(applyFalloff(falloff, 0.5f)));
        CHECK(weightsInUnitRange(selection));
    }
}

TEST_CASE("line angle snapping quantizes the gradient direction to 15 degrees") {
    const Mesh mesh = makeGrid(5, 5);
    const float len = 4.0f;
    const auto dirAt = [len](float degrees) {
        const float a = degrees * kPi / 180.0f;
        return Vec3{len * std::cos(a), len * std::sin(a), 0.0f};
    };
    const auto weightsFor = [&mesh](Vec3 end, bool snapAngle) {
        retopo::SoftSelection selection;
        retopo::LineRegion region;
        region.end = end;
        region.snapAngle = snapAngle;
        region.falloff = retopo::Falloff::Linear;
        retopo::selectLine(mesh, selection, region);
        return std::vector<float>(selection.weights().begin(), selection.weights().end());
    };

    // A 20-degree drag snaps onto the 15-degree increment.
    const std::vector<float> snapped20 = weightsFor(dirAt(20.0f), true);
    const std::vector<float> exact15 = weightsFor(dirAt(15.0f), false);
    REQUIRE(snapped20.size() == exact15.size());
    for (std::size_t i = 0; i < snapped20.size(); ++i) {
        CHECK(snapped20[i] == doctest::Approx(exact15[i]).epsilon(1e-5));
    }

    // An exact increment is a fixed point of the snap.
    const std::vector<float> snapped15 = weightsFor(dirAt(15.0f), true);
    for (std::size_t i = 0; i < snapped15.size(); ++i) {
        CHECK(snapped15[i] == doctest::Approx(exact15[i]).epsilon(1e-5));
    }

    // Without snapping the raw 20-degree direction is used verbatim.
    const std::vector<float> raw20 = weightsFor(dirAt(20.0f), false);
    bool differs = false;
    for (std::size_t i = 0; i < raw20.size(); ++i) {
        differs = differs || std::abs(raw20[i] - exact15[i]) > 1e-3f;
    }
    CHECK(differs);
}

// ---- gradient region selection: sphere -------------------------------------

TEST_CASE("sphere region falls off radially and stops at the radius") {
    const Mesh mesh = makeGrid(7, 2);
    retopo::SoftSelection selection;
    retopo::SphereRegion region;
    region.center = {0.0f, 0.0f, 0.0f};
    region.radius = 3.0f;
    retopo::selectSphere(mesh, selection, region);

    CHECK(selection.weight(VertexId{0}) == 1.0f);
    CHECK(selection.weight(VertexId{1}) > selection.weight(VertexId{2}));
    CHECK(selection.weight(VertexId{3}) == 0.0f);  // exactly at the radius
    CHECK(selection.weight(VertexId{4}) == 0.0f);  // beyond it
    CHECK(weightsInUnitRange(selection));
}

// ---- gradient region selection: painted ------------------------------------

TEST_CASE("painted weights accumulate and a subtract stroke reduces them") {
    const Mesh mesh = makeGrid(9, 3);
    const retopo::PaintBrush brush{2.5f, false, retopo::Falloff::Smooth};

    // One dab alone versus two overlapping dabs: the overlap accumulates.
    retopo::SoftSelection single;
    retopo::paintSelection(mesh, single, {{3.0f, 1.0f, 0.0f}, 0.5f}, brush);
    const float soloOverlap = single.weight(VertexId{9 + 4});

    retopo::SoftSelection both;
    retopo::paintSelection(mesh, both, {{3.0f, 1.0f, 0.0f}, 0.5f}, brush);
    retopo::paintSelection(mesh, both, {{5.0f, 1.0f, 0.0f}, 0.5f}, brush);
    const float overlap = both.weight(VertexId{9 + 4});  // covered by both dabs
    CHECK(overlap > soloOverlap);
    CHECK(overlap <= 1.0f);
    CHECK(both.weight(VertexId{9 + 0}) == 0.0f);  // outside every dab
    CHECK(both.weight(VertexId{9 + 8}) == 0.0f);
    CHECK(weightsInUnitRange(both));

    // Repeated dabs saturate at exactly 1 and never exceed it.
    for (int i = 0; i < 8; ++i) {
        retopo::paintSelection(mesh, both, {{4.0f, 1.0f, 0.0f}, 0.5f}, brush);
    }
    CHECK(both.weight(VertexId{9 + 4}) == 1.0f);
    CHECK(weightsInUnitRange(both));

    // A subtract dab lowers the covered weights and clamps at exactly 0.
    const retopo::PaintBrush eraser{2.5f, true, retopo::Falloff::Smooth};
    retopo::paintSelection(mesh, both, {{4.0f, 1.0f, 0.0f}, 0.25f}, eraser);
    CHECK(both.weight(VertexId{9 + 4}) < 1.0f);
    CHECK(both.weight(VertexId{9 + 4}) > 0.0f);
    for (int i = 0; i < 10; ++i) {
        retopo::paintSelection(mesh, both, {{4.0f, 1.0f, 0.0f}, 1.0f}, eraser);
    }
    CHECK(both.weight(VertexId{9 + 4}) == 0.0f);
    CHECK(weightsInUnitRange(both));
}

TEST_CASE("the stroke route paints the same region as the per-dab route") {
    const Mesh mesh = makeGrid(9, 3);
    const retopo::PaintBrush brush{2.0f, false, retopo::Falloff::Smooth};
    const std::vector<retopo::PaintDab> dabs = {
        {{2.0f, 1.0f, 0.0f}, 0.4f}, {{4.0f, 1.0f, 0.0f}, 0.4f}, {{6.0f, 1.0f, 0.0f}, 0.4f}};

    retopo::SoftSelection perDab;
    for (const retopo::PaintDab& dab : dabs) {
        retopo::paintSelection(mesh, perDab, dab, brush);
    }
    retopo::SoftSelection stroke;
    retopo::paintSelectionStroke(mesh, stroke, dabs, brush);

    REQUIRE(perDab.size() == stroke.size());
    bool anyPainted = false;
    for (std::size_t i = 0; i < perDab.size(); ++i) {
        CHECK(stroke.weights()[i] == doctest::Approx(perDab.weights()[i]));
        anyPainted = anyPainted || perDab.weights()[i] > 0.0f;
    }
    CHECK(anyPainted);
}

// Regression: the coverage test was `if (d >= brush.radius) continue;`, which
// is FALSE when d is NaN, so a dab with a non-finite center or pressure
// "covered" every vertex whatever the radius. The NaN amount was then
// sanitized to 0 on the way into the field — one stray dab (a viewport
// ray-miss unprojects to NaN) silently erased the whole painted selection.
TEST_CASE("a dab that describes no region cannot wipe the painted field") {
    const float nan = std::numeric_limits<float>::quiet_NaN();
    const Mesh mesh = makeGrid(9, 3);
    const retopo::PaintBrush brush{2.5f, false, retopo::Falloff::Smooth};

    retopo::SoftSelection selection;
    retopo::paintSelection(mesh, selection, {{4.0f, 1.0f, 0.0f}, 1.0f}, brush);
    const std::vector<float> painted(selection.weights().begin(), selection.weights().end());
    REQUIRE(painted[9 + 4] > 0.0f);

    // A radius of 0.5 can physically touch at most one vertex, so a wipe is
    // unmistakable.
    const retopo::PaintBrush pinBrush{0.5f, false, retopo::Falloff::Smooth};
    retopo::paintSelection(mesh, selection, {{nan, nan, nan}, 1.0f}, pinBrush);
    retopo::paintSelection(mesh, selection, {{4.0f, 1.0f, 0.0f}, nan}, pinBrush);
    const std::vector<float> afterDabs(selection.weights().begin(), selection.weights().end());
    CHECK(afterDabs == painted);

    // A bad first sample must not poison the stroke's bounding box either: the
    // good sample still paints and nothing else changes.
    retopo::SoftSelection stroke;
    const std::vector<retopo::PaintDab> mixed = {{{nan, nan, nan}, 1.0f},
                                                 {{4.0f, 1.0f, 0.0f}, 1.0f}};
    retopo::paintSelectionStroke(mesh, stroke, mixed, brush);
    REQUIRE(stroke.size() == painted.size());
    for (std::size_t i = 0; i < painted.size(); ++i) {
        CHECK(stroke.weights()[i] == doctest::Approx(painted[i]));
    }

    // Nothing but non-finite samples means nothing to paint.
    retopo::SoftSelection none;
    retopo::paintSelection(mesh, none, {{4.0f, 1.0f, 0.0f}, 1.0f}, brush);
    const std::vector<retopo::PaintDab> allBad = {{{nan, 0.0f, 0.0f}, 1.0f}};
    retopo::paintSelectionStroke(mesh, none, allBad, brush);
    for (std::size_t i = 0; i < painted.size(); ++i) {
        CHECK(none.weights()[i] == doctest::Approx(painted[i]));
    }
}

// Regression: vertex ids are recycled from the mesh's free list, so a weight
// left on a vertex that died was inherited by the next vertex created in that
// slot — brand-new geometry born selected and dragged by the weighted
// transform.
TEST_CASE("a recycled vertex id starts unselected") {
    Mesh mesh = makeGrid(2, 2);  // one quad, vertex ids 0..3
    retopo::SoftSelection selection;
    selection.resizeFor(mesh);
    for (Index i = 0; i < mesh.vertexCapacity(); ++i) {
        selection.setWeight(VertexId{i}, 1.0f);
    }

    mesh.removeFace(cyber::FaceId{0});
    for (Index i = 0; i < mesh.vertexCapacity(); ++i) {
        const VertexId v{i};
        if (mesh.isAlive(v) && mesh.vertexEdges(v).empty()) {
            mesh.removeIsolatedVertex(v);
        }
    }
    REQUIRE(mesh.vertexCount() == 0u);

    selection.dropDead(mesh);
    for (const float w : selection.weights()) {
        CHECK(w == 0.0f);
    }

    const VertexId reused = mesh.addVertex(Vec3{100.0f, 100.0f, 0.0f});
    CHECK(selection.weight(reused) == 0.0f);
    retopo::Affine move;
    move.translation = Vec3{0.0f, 0.0f, 10.0f};
    retopo::transformWeighted(mesh, selection, move, nullptr, nullptr);
    CHECK(mesh.position(reused).z == 0.0f);

    // Weights past the mesh's capacity belong to no vertex at all: a caller
    // that pushed in an oversized field must not have it land on the next
    // vertices the mesh grows into.
    std::vector<float> oversized(mesh.vertexCapacity() + 4u, 1.0f);
    retopo::dropDeadWeights(mesh, oversized);
    for (std::size_t i = 0; i < oversized.size(); ++i) {
        const bool alive = mesh.isAlive(VertexId{static_cast<Index>(i)});
        CHECK(oversized[i] == (alive ? 1.0f : 0.0f));
    }
}

// ---- selection operations --------------------------------------------------

TEST_CASE("clear and invert reshape the whole weight field") {
    const Mesh mesh = makeGrid(5, 3);
    retopo::SoftSelection selection;
    retopo::SphereRegion region;
    region.center = {0.0f, 0.0f, 0.0f};
    region.radius = 3.0f;
    retopo::selectSphere(mesh, selection, region);

    const float before = selection.weight(VertexId{0});
    retopo::invertSelection(mesh, selection);
    CHECK(selection.weight(VertexId{0}) == doctest::Approx(1.0f - before));
    CHECK(weightsInUnitRange(selection));

    retopo::clearSelection(selection);
    for (const float w : selection.weights()) {
        CHECK(w == 0.0f);
    }
}

TEST_CASE("expand grows the selection by a ring and contract shrinks it back") {
    const Mesh mesh = makeGrid(7, 7);
    const VertexId seed{7 * 3 + 3};  // interior vertex, valence 4
    retopo::SoftSelection selection;
    selection.resizeFor(mesh);
    selection.setWeight(seed, 1.0f);

    const auto countFull = [&selection] {
        std::size_t n = 0;
        for (const float w : selection.weights()) {
            if (w == 1.0f) {
                ++n;
            }
        }
        return n;
    };
    REQUIRE(countFull() == 1);

    retopo::expandSelection(mesh, selection, 1);
    CHECK(countFull() == 5);  // the seed plus its four neighbours
    retopo::expandSelection(mesh, selection, 2);
    CHECK(countFull() > 5);

    retopo::contractSelection(mesh, selection, 3);
    CHECK(countFull() == 1);
    CHECK(selection.weight(seed) == 1.0f);
    CHECK(weightsInUnitRange(selection));
}

TEST_CASE("smoothing widens a hard-edged selection without moving geometry") {
    Mesh mesh = makeGrid(41, 3);
    const std::vector<Vec3> before = snapshot(mesh);

    retopo::SoftSelection selection;
    selection.resizeFor(mesh);
    for (Index i = 0; i < mesh.vertexCapacity(); ++i) {
        selection.setWeight(VertexId{i}, (i % 41u) <= 20u ? 1.0f : 0.0f);
    }
    REQUIRE(countStrictlyFractional(selection) == 0);

    retopo::smoothSelection(mesh, selection, 1);
    const std::size_t after1 = countStrictlyFractional(selection);
    retopo::smoothSelection(mesh, selection, 5);
    const std::size_t after5 = countStrictlyFractional(selection);
    retopo::smoothSelection(mesh, selection, 10);
    const std::size_t after10 = countStrictlyFractional(selection);

    CHECK(after1 > 0);
    CHECK(after5 > after1);
    CHECK(after10 > after5);
    CHECK(weightsInUnitRange(selection));

    // The op touches weights only: every position is bit-identical.
    const std::vector<Vec3> after = snapshot(mesh);
    CHECK(after == before);
}

// ---- weighted transform with surface glue ----------------------------------

TEST_CASE(
    "weighted scale tapers along the gradient and every affected vertex stays on the "
    "Target") {
    const Mesh target = makeCone(64, 48, -0.5f, 3.0f);
    const retopo::SurfaceSnapper snap(target);
    REQUIRE_FALSE(snap.empty());

    Mesh edit = makeCone(12, 4, 0.0f, 2.0f);
    // Start exactly glued to the Target so the taper measurement is about the
    // transform, not about the initial projection.
    for (Index i = 0; i < edit.vertexCapacity(); ++i) {
        const VertexId v{i};
        edit.setPosition(v, snap.snapToSurface(edit.position(v)).point);
    }
    const std::vector<Vec3> before = snapshot(edit);

    retopo::SoftSelection selection;
    retopo::LineRegion region;
    region.anchor = {0.0f, 0.0f, 0.0f};  // limb base: weight 0
    region.end = {0.0f, 2.0f, 0.0f};     // limb tip: weight 1
    region.falloff = retopo::Falloff::Linear;
    retopo::selectLine(edit, selection, region);

    const retopo::ResnapReport report = retopo::transformWeighted(
        edit, selection, retopo::Affine::scaling({0.6f, 1.0f, 0.6f}), &snap);

    std::size_t affected = 0;
    for (Index i = 0; i < edit.vertexCapacity(); ++i) {
        if (selection.weight(VertexId{i}) > 0.0f) {
            ++affected;
        }
    }
    CHECK(report.moved == affected);
    CHECK(report.resnapped > 0);
    CHECK(report.maxSnapDistance > 0.0f);

    // Every affected vertex lies on the Target after the single operation —
    // no separate snap-all pass was run.
    float worstOffSurface = 0.0f;
    for (Index i = 0; i < edit.vertexCapacity(); ++i) {
        const VertexId v{i};
        if (selection.weight(v) <= 0.0f) {
            continue;
        }
        worstOffSurface =
            std::max(worstOffSurface, snap.snapToSurface(edit.position(v)).distanceSquared);
    }
    CHECK(worstOffSurface <= 1e-8f);

    // The limb narrows more where the gradient is stronger: mean radial
    // reduction per ring is strictly increasing from base to tip.
    std::vector<float> reductionPerRing;
    for (int r = 0; r <= 4; ++r) {
        float sum = 0.0f;
        for (int s = 0; s < 12; ++s) {
            const Index i = static_cast<Index>(r * 12 + s);
            sum += radialDistance(before[i]) - radialDistance(edit.position(VertexId{i}));
        }
        reductionPerRing.push_back(sum / 12.0f);
    }
    CHECK(reductionPerRing[0] == doctest::Approx(0.0f));  // base ring has weight 0
    for (std::size_t r = 0; r + 1 < reductionPerRing.size(); ++r) {
        CHECK(reductionPerRing[r + 1] > reductionPerRing[r]);
    }
}

TEST_CASE("weighted relax keeps the affected vertices on the Target in one pass") {
    const Mesh target = makeCone(64, 48, -0.5f, 3.0f);
    const retopo::SurfaceSnapper snap(target);
    Mesh edit = makeCone(12, 6, 0.0f, 2.0f);
    for (Index i = 0; i < edit.vertexCapacity(); ++i) {
        const VertexId v{i};
        edit.setPosition(v, snap.snapToSurface(edit.position(v)).point + Vec3{0.0f, 0.0f, 0.05f});
    }

    retopo::SoftSelection selection;
    retopo::LineRegion region;
    region.anchor = {0.0f, 0.0f, 0.0f};
    region.end = {0.0f, 2.0f, 0.0f};
    region.falloff = retopo::Falloff::Linear;
    retopo::selectLine(edit, selection, region);

    retopo::RelaxParams params;
    params.strength = 0.5f;
    params.iterations = 2;
    params.autoPinCorners = false;
    const retopo::ResnapReport report = retopo::relaxWeighted(edit, selection, params, &snap);
    CHECK(report.moved > 0);

    for (Index i = 0; i < edit.vertexCapacity(); ++i) {
        const VertexId v{i};
        if (selection.weight(v) <= 0.0f) {
            continue;
        }
        CHECK(snap.snapToSurface(edit.position(v)).distanceSquared <= 1e-8f);
    }
}

TEST_CASE("zero-weight vertices are bit-identical after any weighted operation") {
    const Mesh target = makeGrid(9, 9);  // Target plane at z = 0
    const retopo::SurfaceSnapper snap(target);
    REQUIRE_FALSE(snap.empty());

    // The EditMesh floats above the Target, so a stray re-snap of an untouched
    // vertex would be plainly visible.
    const Mesh pristine = makeGrid(9, 9, 0.3f);

    retopo::SoftSelection selection;
    selection.resizeFor(pristine);
    for (Index i = 0; i < pristine.vertexCapacity(); ++i) {
        selection.setWeight(VertexId{i}, (i % 9u) >= 5u ? 1.0f : 0.0f);
    }

    retopo::PinSet pins;
    const VertexId pinned{9 * 4 + 7};  // inside the selection, but pinned
    pins.pin(pinned);

    const retopo::Affine transforms[] = {
        retopo::Affine::translating({0.0f, 0.0f, 1.5f}),
        retopo::Affine::rotating({0.0f, 0.0f, 1.0f}, 0.4f),
        retopo::Affine::scaling({1.7f, 1.7f, 1.0f}),
    };
    const std::vector<Vec3> before = snapshot(pristine);

    for (const retopo::Affine& xf : transforms) {
        for (const bool useSnapper : {false, true}) {
            Mesh edit = pristine;
            retopo::transformWeighted(edit, selection, xf, useSnapper ? &snap : nullptr, &pins);
            for (Index i = 0; i < edit.vertexCapacity(); ++i) {
                const VertexId v{i};
                const bool immune = selection.weight(v) <= 0.0f || v.value == pinned.value;
                if (immune) {
                    CHECK(edit.position(v) == before[i]);
                } else {
                    CHECK_FALSE(edit.position(v) == before[i]);
                }
            }
        }
    }

    retopo::RelaxParams params;
    params.strength = 0.7f;
    params.iterations = 3;
    params.autoPinCorners = false;
    for (const bool useSnapper : {false, true}) {
        Mesh edit = pristine;
        retopo::relaxWeighted(edit, selection, params, useSnapper ? &snap : nullptr, &pins);
        for (Index i = 0; i < edit.vertexCapacity(); ++i) {
            const VertexId v{i};
            if (selection.weight(v) <= 0.0f || v.value == pinned.value) {
                CHECK(edit.position(v) == before[i]);
            }
        }
    }
}

TEST_CASE("weighted relax reproduces plain relax when every weight is 1") {
    const Mesh pristine = makeGrid(7, 7, 0.2f);
    retopo::SoftSelection selection;
    selection.resizeFor(pristine);
    for (Index i = 0; i < pristine.vertexCapacity(); ++i) {
        selection.setWeight(VertexId{i}, 1.0f);
    }

    retopo::RelaxParams params;
    params.strength = 0.4f;
    params.iterations = 3;

    Mesh plain = pristine;
    retopo::relax(plain, params);
    Mesh weighted = pristine;
    retopo::relaxWeighted(weighted, selection, params);

    CHECK(snapshot(plain) == snapshot(weighted));
}
