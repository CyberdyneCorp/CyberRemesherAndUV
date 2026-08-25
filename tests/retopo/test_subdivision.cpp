// Subdivision modes (manual-retopology spec, "Whole-mesh commands"): the
// historical linear path and the opt-in Catmull-Clark smooth path.
#include <doctest.h>

#include <cmath>
#include <cstdint>
#include <cstring>
#include <vector>

#include "cyber/retopo/commands.hpp"
#include "cyber/retopo/subdivision.hpp"

using cyber::EdgeId;
using cyber::FaceId;
using cyber::Index;
using cyber::Mesh;
using cyber::Vec3;
using cyber::VertexId;
namespace retopo = cyber::retopo;

namespace {

// A unit cube, quads, outward-wound.
Mesh makeCube() {
    const std::vector<Vec3> p = {{-0.5f, -0.5f, -0.5f}, {0.5f, -0.5f, -0.5f}, {0.5f, 0.5f, -0.5f},
                                 {-0.5f, 0.5f, -0.5f},  {-0.5f, -0.5f, 0.5f}, {0.5f, -0.5f, 0.5f},
                                 {0.5f, 0.5f, 0.5f},    {-0.5f, 0.5f, 0.5f}};
    const std::vector<std::vector<Index>> f = {{0, 3, 2, 1}, {4, 5, 6, 7}, {0, 1, 5, 4},
                                               {1, 2, 6, 5}, {2, 3, 7, 6}, {3, 0, 4, 7}};
    return Mesh::fromIndexed(p, f);
}

// An OPEN planar quad patch: the n x n grid over [0,n]^2 in the z = 0 plane.
Mesh makeOpenPatch(int n) {
    std::vector<Vec3> p;
    for (int j = 0; j <= n; ++j) {
        for (int i = 0; i <= n; ++i) {
            p.push_back({static_cast<float>(i), static_cast<float>(j), 0.0f});
        }
    }
    std::vector<std::vector<Index>> f;
    const Index stride = static_cast<Index>(n) + 1;
    for (Index j = 0; j < static_cast<Index>(n); ++j) {
        for (Index i = 0; i < static_cast<Index>(n); ++i) {
            const Index a = j * stride + i;
            f.push_back({a, a + 1, a + stride + 1, a + stride});
        }
    }
    return Mesh::fromIndexed(p, f);
}

// A TUBE open at both ends: `seg` segments around, rings at z = 0, 1, 2.
Mesh makeOpenTube(Index seg) {
    std::vector<Vec3> p;
    for (Index k = 0; k < 3; ++k) {
        for (Index i = 0; i < seg; ++i) {
            const float a = 2.0f * cyber::kPi * static_cast<float>(i) / static_cast<float>(seg);
            p.push_back({std::cos(a), std::sin(a), static_cast<float>(k)});
        }
    }
    std::vector<std::vector<Index>> f;
    for (Index k = 0; k < 2; ++k) {
        for (Index i = 0; i < seg; ++i) {
            const Index a = k * seg + i;
            const Index b = k * seg + (i + 1) % seg;
            f.push_back({a, b, b + seg, a + seg});
        }
    }
    return Mesh::fromIndexed(p, f);
}

Mesh makeTriangle() {
    return Mesh::fromIndexed(
        std::vector<Vec3>{{0.0f, 0.0f, 0.0f}, {1.0f, 0.0f, 0.0f}, {0.0f, 1.0f, 0.0f}},
        std::vector<std::vector<Index>>{{0, 1, 2}});
}

// FNV-1a over the compact indexed export: every position's float BITS and
// every face's vertex list, in id order. Two meshes hash equal only if they
// are bit-for-bit the same geometry in the same order.
void hashBytes(std::uint64_t& h, const void* data, std::size_t n) {
    const auto* p = static_cast<const unsigned char*>(data);
    for (std::size_t i = 0; i < n; ++i) {
        h ^= p[i];
        h *= 1099511628211ull;
    }
}

std::uint64_t hashMesh(const Mesh& mesh) {
    std::vector<Vec3> positions;
    std::vector<std::vector<Index>> faces;
    mesh.toIndexed(positions, faces);
    std::uint64_t h = 1469598103934665603ull;
    for (const Vec3& p : positions) {
        for (const float c : {p.x, p.y, p.z}) {
            std::uint32_t bits = 0;
            std::memcpy(&bits, &c, sizeof(bits));
            hashBytes(h, &bits, sizeof(bits));
        }
    }
    for (const std::vector<Index>& face : faces) {
        const auto n = static_cast<std::uint32_t>(face.size());
        hashBytes(h, &n, sizeof(n));
        for (const Index i : face) {
            hashBytes(h, &i, sizeof(i));
        }
    }
    return h;
}

// Largest angle (degrees) between the normals of the two faces sharing an
// interior edge: 90 on a cube, 0 on a plane. This is the faceting measure —
// what "smooth subdivision converges and linear does not" means numerically.
float maxInteriorDihedral(const Mesh& mesh) {
    float worst = 0.0f;
    for (Index ei = 0; ei < mesh.edgeCapacity(); ++ei) {
        const EdgeId e{ei};
        if (!mesh.isAlive(e) || mesh.edgeFaceCount(e) != 2) {
            continue;
        }
        const std::vector<FaceId> faces = mesh.edgeFaces(e);
        const float c =
            std::fmax(-1.0f, std::fmin(1.0f, dot(normalized(mesh.faceNormal(faces[0])),
                                                 normalized(mesh.faceNormal(faces[1])))));
        worst = std::fmax(worst, cyber::radiansToDegrees(std::acos(c)));
    }
    return worst;
}

// (max - min) / mean of the vertex distances to the origin: 0 for a sphere
// centred there, large for anything faceted.
float radiusSpread(const Mesh& mesh) {
    float lo = 1e30f;
    float hi = 0.0f;
    double sum = 0.0;
    std::size_t n = 0;
    for (Index vi = 0; vi < mesh.vertexCapacity(); ++vi) {
        const VertexId v{vi};
        if (!mesh.isAlive(v)) {
            continue;
        }
        const float r = length(mesh.position(v));
        lo = std::fmin(lo, r);
        hi = std::fmax(hi, r);
        sum += static_cast<double>(r);
        ++n;
    }
    return (hi - lo) / static_cast<float>(sum / static_cast<double>(n));
}

struct Bounds {
    Vec3 lo{1e30f, 1e30f, 1e30f};
    Vec3 hi{-1e30f, -1e30f, -1e30f};
};

Bounds boundsOf(const Mesh& mesh) {
    Bounds b;
    for (Index vi = 0; vi < mesh.vertexCapacity(); ++vi) {
        const VertexId v{vi};
        if (mesh.isAlive(v)) {
            b.lo = min(b.lo, mesh.position(v));
            b.hi = max(b.hi, mesh.position(v));
        }
    }
    return b;
}

}  // namespace

// The linear path is the default and must never move: these hashes were taken
// from the build IMMEDIATELY BEFORE Catmull-Clark was added, so a single
// changed float bit or a reordered face anywhere in the linear path breaks
// them. Nothing about the smooth mode may be allowed to disturb it.
TEST_CASE("linear subdivision is bit-identical to the pre-Catmull-Clark engine") {
    Mesh cube = makeCube();
    cube = retopo::subdivide(retopo::subdivide(cube, retopo::SubdivisionMode::Linear),
                             retopo::SubdivisionMode::Linear);
    CHECK(hashMesh(cube) == 0xc68375f95dbc82f3ULL);

    Mesh patch = makeOpenPatch(2);
    patch = retopo::subdivide(retopo::subdivide(patch, retopo::SubdivisionMode::Linear),
                              retopo::SubdivisionMode::Linear);
    CHECK(hashMesh(patch) == 0xc0f5e0c5a9272c2fULL);

    CHECK(hashMesh(retopo::subdivide(makeTriangle(), retopo::SubdivisionMode::Linear)) ==
          0xe66267aad82bd4a2ULL);

    // The mode-less entry points are the linear mode, not a third code path.
    Mesh viaCommand = makeCube();
    retopo::subdivideAll(viaCommand);
    retopo::subdivideAll(viaCommand);
    CHECK(hashMesh(viaCommand) == 0xc68375f95dbc82f3ULL);
    CHECK(hashMesh(makeCube().linearSubdivide().linearSubdivide()) == 0xc68375f95dbc82f3ULL);
}

TEST_CASE("both modes build the same topology") {
    const Mesh cube = makeCube();
    const Mesh linear = retopo::subdivide(cube, retopo::SubdivisionMode::Linear);
    const Mesh smooth = retopo::subdivide(cube, retopo::SubdivisionMode::CatmullClark);
    CHECK(smooth.faceCount() == linear.faceCount());
    CHECK(smooth.vertexCount() == linear.vertexCount());
    CHECK(smooth.edgeCount() == linear.edgeCount());
    CHECK(smooth.validate().empty());
    // Catmull-Clark writes positions and nothing else, so the topology hash
    // differing is entirely down to the coordinates.
    CHECK(hashMesh(smooth) != hashMesh(linear));
}

TEST_CASE("Catmull-Clark rounds a cube off; linear leaves it faceted") {
    // Faceting, level by level. Linear NEVER improves: the cube's original
    // creases stay at exactly 90 degrees however many times it runs.
    Mesh linear = makeCube();
    for (int i = 0; i < 4; ++i) {
        linear = retopo::subdivide(linear, retopo::SubdivisionMode::Linear);
        CHECK(maxInteriorDihedral(linear) == doctest::Approx(90.0f).epsilon(1e-4));
    }

    // Catmull-Clark roughly halves the worst crease angle every level.
    // Measured (degrees): 90 -> 43.34 -> 26.46 -> 13.93 -> 7.06, i.e. ratios
    // 0.48, 0.61, 0.53, 0.51. The 0.65 bound is loose enough around the
    // slowest of those that float noise cannot flake it, and still fails
    // outright for anything that stops converging.
    Mesh smooth = makeCube();
    float previous = maxInteriorDihedral(smooth);
    CHECK(previous == doctest::Approx(90.0f).epsilon(1e-4));
    for (int level = 1; level <= 4; ++level) {
        smooth = retopo::subdivide(smooth, retopo::SubdivisionMode::CatmullClark);
        const float current = maxInteriorDihedral(smooth);
        CHECK(current < previous * 0.65f);
        previous = current;
    }
    CHECK(previous < 8.0f);  // measured 7.06 at level 4

    // ...and the result is sphere-ish, which the faceted one is not. The
    // vertex radii of the cube's linear subdivision span 0.5 (face centres) to
    // 0.866 (corners) — a spread of 0.568 of the mean — while Catmull-Clark
    // pulls them into the band [0.4246, 0.4369], a spread of 0.0285: a 19x
    // tighter shell, and the residual is the real Catmull-Clark limit surface
    // of a cube, which is not exactly a sphere.
    Mesh linear3 = makeCube();
    Mesh smooth3 = makeCube();
    for (int i = 0; i < 3; ++i) {
        linear3 = retopo::subdivide(linear3, retopo::SubdivisionMode::Linear);
        smooth3 = retopo::subdivide(smooth3, retopo::SubdivisionMode::CatmullClark);
    }
    CHECK(radiusSpread(linear3) == doctest::Approx(0.5675f).epsilon(0.01));
    CHECK(radiusSpread(smooth3) < 0.03f);
    CHECK(radiusSpread(smooth3) < radiusSpread(linear3) / 15.0f);
}

TEST_CASE("an open patch keeps its border instead of shrinking inward") {
    // Every border edge is a boundary edge, so the sharp-crease rule applies:
    // a straight border stays exactly on its line and the four valence-2
    // patch corners do not move at all. Under the smooth interior rule the
    // border would be pulled toward the patch centre by a measurable fraction
    // of a cell on every level.
    Mesh patch = makeOpenPatch(4);
    const Bounds before = boundsOf(patch);
    for (int i = 0; i < 3; ++i) {
        patch = retopo::subdivide(patch, retopo::SubdivisionMode::CatmullClark);
    }
    const Bounds after = boundsOf(patch);
    CHECK(std::fabs(after.lo.x - before.lo.x) < 1e-6f);
    CHECK(std::fabs(after.hi.x - before.hi.x) < 1e-6f);
    CHECK(std::fabs(after.lo.y - before.lo.y) < 1e-6f);
    CHECK(std::fabs(after.hi.y - before.hi.y) < 1e-6f);

    // A planar patch has a planar limit surface: nothing may leave z = 0
    // either, in the interior or on the border.
    for (Index vi = 0; vi < patch.vertexCapacity(); ++vi) {
        const VertexId v{vi};
        if (patch.isAlive(v)) {
            CHECK(std::fabs(patch.position(v).z) < 1e-6f);
        }
    }

    // Every vertex ON the border is still on the square's outline, so the
    // border did not bow inward between the corners.
    std::size_t borderVertices = 0;
    for (Index ei = 0; ei < patch.edgeCapacity(); ++ei) {
        const EdgeId e{ei};
        if (!patch.isAlive(e) || !patch.isBoundaryEdge(e)) {
            continue;
        }
        const auto [a, b] = patch.edgeVertices(e);
        for (const VertexId v : {a, b}) {
            const Vec3 p = patch.position(v);
            const bool onOutline = std::fabs(p.x - 0.0f) < 1e-6f || std::fabs(p.x - 4.0f) < 1e-6f ||
                                   std::fabs(p.y - 0.0f) < 1e-6f || std::fabs(p.y - 4.0f) < 1e-6f;
            CHECK(onOutline);
            ++borderVertices;
        }
    }
    CHECK(borderVertices > 0);
}

TEST_CASE("an open tube's boundary rings do not creep along the axis") {
    // The curved case the planar patch cannot show: each end ring is a closed
    // crease, so it stays in its own plane (z exactly 0 and 2) while the
    // surface between them smooths. With the smooth interior rule the rings
    // would be dragged toward the tube's middle, shortening it every level.
    Mesh tube = makeOpenTube(8);
    for (int level = 1; level <= 3; ++level) {
        tube = retopo::subdivide(tube, retopo::SubdivisionMode::CatmullClark);
        const Bounds b = boundsOf(tube);
        CHECK(std::fabs(b.lo.z - 0.0f) < 1e-6f);
        CHECK(std::fabs(b.hi.z - 2.0f) < 1e-6f);
    }
    // The rings still converge to their own limit CURVE in-plane — the cubic
    // B-spline of the source octagon, radius ~0.903 — rather than collapsing.
    for (Index vi = 0; vi < tube.vertexCapacity(); ++vi) {
        const VertexId v{vi};
        if (!tube.isAlive(v)) {
            continue;
        }
        const Vec3 p = tube.position(v);
        CHECK(std::sqrt(p.x * p.x + p.y * p.y) > 0.9f);
    }
}

TEST_CASE("tagged feature edges hold the crease sharp") {
    // relaxQuadMesh() in the pipeline freezes "feature or boundary" vertices;
    // Catmull-Clark uses the same predicate to pick the sharp rule. A cube
    // with every edge tagged is therefore returned as an exact cube: corners
    // do not move, edge points sit at the midpoints, face points at the
    // centroids.
    Mesh cube = makeCube();
    cube.tagFeatureEdges(90.0f);
    const Mesh sharp = retopo::subdivide(cube, retopo::SubdivisionMode::CatmullClark);

    CHECK(maxInteriorDihedral(sharp) == doctest::Approx(90.0f).epsilon(1e-4));
    for (Index vi = 0; vi < sharp.vertexCapacity(); ++vi) {
        const VertexId v{vi};
        if (!sharp.isAlive(v)) {
            continue;
        }
        const Vec3 p = sharp.position(v);
        const float extent = std::fmax(std::fabs(p.x), std::fmax(std::fabs(p.y), std::fabs(p.z)));
        CHECK(std::fabs(extent - 0.5f) < 1e-6f);
    }
    // The eight original corners survive at full radius; the untagged cube
    // pulls them in to 0.481.
    CHECK(radiusSpread(sharp) ==
          doctest::Approx(radiusSpread(makeCube().linearSubdivide())).epsilon(1e-4));

    // Same mesh, no tags: the smooth rules run and the corners move.
    const Mesh smooth = retopo::subdivide(makeCube(), retopo::SubdivisionMode::CatmullClark);
    CHECK(maxInteriorDihedral(smooth) < 45.0f);
}

TEST_CASE("Catmull-Clark leaves attributes to the shared topology pass") {
    // Positions are the only thing the smooth rules write, so the propagated
    // attributes must be identical to the linear path's.
    Mesh cube = makeCube();
    std::vector<Vec3>& color = cube.vertexAttributes().create<Vec3>("color");
    for (Index vi = 0; vi < cube.vertexCapacity(); ++vi) {
        const VertexId v{vi};
        if (cube.isAlive(v)) {
            color[vi] = cube.position(v);
        }
    }
    const Mesh linear = retopo::subdivide(cube, retopo::SubdivisionMode::Linear);
    const Mesh smooth = retopo::subdivide(cube, retopo::SubdivisionMode::CatmullClark);
    REQUIRE(smooth.vertexCount() == linear.vertexCount());
    const std::vector<Vec3>* a = linear.vertexAttributes().find<Vec3>("color");
    const std::vector<Vec3>* b = smooth.vertexAttributes().find<Vec3>("color");
    REQUIRE(a != nullptr);
    REQUIRE(b != nullptr);
    for (Index vi = 0; vi < smooth.vertexCapacity(); ++vi) {
        const VertexId v{vi};
        if (smooth.isAlive(v)) {
            CHECK((*a)[vi] == (*b)[vi]);
        }
    }
}
