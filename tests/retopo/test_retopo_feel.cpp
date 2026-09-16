#include <doctest.h>

#include <array>
#include <cmath>
#include <utility>
#include <vector>

#include "cyber/core/mesh.hpp"
#include "cyber/retopo/loops.hpp"
#include "cyber/retopo/pins.hpp"
#include "cyber/retopo/relax.hpp"

using cyber::EdgeId;
using cyber::Index;
using cyber::Mesh;
using cyber::Vec3;
using cyber::VertexId;
namespace retopo = cyber::retopo;

namespace {

constexpr float kPi = 3.14159265358979323846f;

// (nx+1) x (ny+1) vertex grid of quads in the XY plane, row-major, CCW winding.
struct Grid {
    Mesh mesh;
    int nx = 0;
    int ny = 0;
    [[nodiscard]] VertexId at(int i, int j) const {
        return VertexId{static_cast<Index>(j * (nx + 1) + i)};
    }
};

Grid grid(int nx, int ny, float spacing = 1.0f) {
    Grid g;
    g.nx = nx;
    g.ny = ny;
    for (int j = 0; j <= ny; ++j) {
        for (int i = 0; i <= nx; ++i) {
            g.mesh.addVertex(
                Vec3{static_cast<float>(i) * spacing, static_cast<float>(j) * spacing, 0.0f});
        }
    }
    for (int j = 0; j < ny; ++j) {
        for (int i = 0; i < nx; ++i) {
            g.mesh.addFace(std::array<VertexId, 4>{g.at(i, j), g.at(i + 1, j), g.at(i + 1, j + 1),
                                                   g.at(i, j + 1)});
        }
    }
    return g;
}

// A tube of quads about +Y: `around` columns, `along` bands. Loops around the
// tube are CLOSED, which is the case a per-vertex side choice gets wrong.
struct Tube {
    Mesh mesh;
    int around = 0;
    int along = 0;
    [[nodiscard]] VertexId at(int i, int j) const {
        return VertexId{static_cast<Index>(j * around + (i % around))};
    }
};

Tube tube(int around, int along, float radius = 1.0f) {
    Tube t;
    t.around = around;
    t.along = along;
    for (int j = 0; j <= along; ++j) {
        for (int i = 0; i < around; ++i) {
            const float a = 2.0f * kPi * static_cast<float>(i) / static_cast<float>(around);
            t.mesh.addVertex(
                Vec3{radius * std::cos(a), static_cast<float>(j), radius * std::sin(a)});
        }
    }
    for (int j = 0; j < along; ++j) {
        for (int i = 0; i < around; ++i) {
            t.mesh.addFace(std::array<VertexId, 4>{t.at(i, j), t.at(i + 1, j), t.at(i + 1, j + 1),
                                                   t.at(i, j + 1)});
        }
    }
    return t;
}

std::vector<Vec3> snapshot(const Mesh& mesh) {
    std::vector<Vec3> out;
    for (Index v = 0; v < mesh.vertexCapacity(); ++v) {
        out.push_back(mesh.position(VertexId{v}));
    }
    return out;
}

bool bitIdentical(Vec3 a, Vec3 b) { return a.x == b.x && a.y == b.y && a.z == b.z; }

}  // namespace

// ---- Loop slide ------------------------------------------------------------

TEST_CASE("loop slide moves every loop vertex the same fraction along its rail") {
    Grid g = grid(6, 6);
    const EdgeId seed = g.mesh.edgeBetween(g.at(2, 3), g.at(3, 3));  // horizontal, row 3
    REQUIRE(seed.valid());
    const auto before = snapshot(g.mesh);

    const retopo::LoopSlideResult r = retopo::slideLoop(g.mesh, seed, 0.25f);

    // Row 3 is a loop of 7 vertices, all with a quad above and below.
    CHECK(r.loopVertices == 7);
    CHECK(r.moved == 7);
    // Every row-3 vertex moves exactly a quarter of the way to ONE of its
    // vertical neighbours, and all of them to the same one.
    float direction = 0.0f;
    for (int i = 0; i <= 6; ++i) {
        const Vec3 p = g.mesh.position(g.at(i, 3));
        CHECK(p.x == doctest::Approx(static_cast<float>(i)));
        const float dy = p.y - 3.0f;
        CHECK(std::abs(dy) == doctest::Approx(0.25f));
        if (i == 0) {
            direction = dy;
        } else {
            CHECK(dy == doctest::Approx(direction));  // same side for every vertex
        }
    }
    // Nothing off the loop moved.
    for (int j = 0; j <= 6; ++j) {
        if (j == 3) {
            continue;
        }
        for (int i = 0; i <= 6; ++i) {
            const VertexId v = g.at(i, j);
            CHECK(bitIdentical(g.mesh.position(v), before[v.value]));
        }
    }
}

TEST_CASE("a slide on a CLOSED loop keeps every vertex on the same side") {
    // The case a per-vertex side choice gets wrong: around a closed ring,
    // choosing each vertex's neighbour independently twists part of the loop
    // up and part of it down.
    Tube t = tube(16, 4);
    const EdgeId seed = t.mesh.edgeBetween(t.at(0, 2), t.at(1, 2));  // ring at height 2
    REQUIRE(seed.valid());

    const retopo::LoopSlideResult r = retopo::slideLoop(t.mesh, seed, 0.3f);
    CHECK(r.loopVertices == 16);
    CHECK(r.moved == 16);

    const float first = t.mesh.position(t.at(0, 2)).y;
    CHECK(std::abs(first - 2.0f) == doctest::Approx(0.3f));
    for (int i = 0; i < 16; ++i) {
        CHECK(t.mesh.position(t.at(i, 2)).y == doctest::Approx(first));
    }
}

TEST_CASE("sliding by t and by -t are mirror images") {
    Grid up = grid(5, 5);
    Grid down = grid(5, 5);
    const EdgeId seedUp = up.mesh.edgeBetween(up.at(1, 2), up.at(2, 2));
    const EdgeId seedDown = down.mesh.edgeBetween(down.at(1, 2), down.at(2, 2));

    CHECK(retopo::slideLoop(up.mesh, seedUp, 0.4f).moved == 6);
    CHECK(retopo::slideLoop(down.mesh, seedDown, -0.4f).moved == 6);

    for (int i = 0; i <= 5; ++i) {
        const float dUp = up.mesh.position(up.at(i, 2)).y - 2.0f;
        const float dDown = down.mesh.position(down.at(i, 2)).y - 2.0f;
        CHECK(std::abs(dUp) == doctest::Approx(0.4f));
        CHECK(dDown == doctest::Approx(-dUp));  // opposite sides, same distance
    }
}

TEST_CASE("a slide does not depend on which loop edge seeds it") {
    Grid a = grid(6, 4);
    Grid b = grid(6, 4);
    const EdgeId first = a.mesh.edgeBetween(a.at(0, 2), a.at(1, 2));
    const EdgeId last = b.mesh.edgeBetween(b.at(5, 2), b.at(6, 2));

    (void)retopo::slideLoop(a.mesh, first, 0.2f);
    (void)retopo::slideLoop(b.mesh, last, 0.2f);
    for (int i = 0; i <= 6; ++i) {
        CHECK(a.mesh.position(a.at(i, 2)).y == doctest::Approx(b.mesh.position(b.at(i, 2)).y));
    }
}

TEST_CASE("a boundary seed slides its own edge, inward only") {
    // What "the loop through a boundary edge" means is decided by
    // edgeLoopFrom, not here: it continues only through valence-4 vertices,
    // and a vertex on an open border has valence 3, so the loop through a
    // boundary edge is that one edge. Slide deliberately uses the same
    // definition as the tag-loop gesture -- tapping a border must not tag one
    // set of vertices and slide another. Pinned here so a change to the loop
    // definition shows up as a change to slide, not a silent one.
    Grid g = grid(4, 3);
    const EdgeId bottom = g.mesh.edgeBetween(g.at(1, 0), g.at(2, 0));
    REQUIRE(bottom.valid());
    REQUIRE(retopo::edgeLoopFrom(g.mesh, bottom).size() == 1);

    Grid other = grid(4, 3);
    // Both snapshots are taken BEFORE either call. Snapshotting the unmoved mesh
    // afterwards would compare it with itself and pass no matter what happened.
    const auto beforeG = snapshot(g.mesh);
    const auto beforeOther = snapshot(other.mesh);
    const retopo::LoopSlideResult plus = retopo::slideLoop(g.mesh, bottom, 0.25f);
    const retopo::LoopSlideResult minus = retopo::slideLoop(
        other.mesh, other.mesh.edgeBetween(other.at(1, 0), other.at(2, 0)), -0.25f);

    CHECK(plus.loopVertices == 2);
    CHECK(minus.loopVertices == 2);
    // Exactly one direction has a quad to slide into.
    CHECK((plus.moved == 0) != (minus.moved == 0));

    const bool plusMoved = plus.moved > 0;
    const Grid& movedGrid = plusMoved ? g : other;
    const Grid& stillGrid = plusMoved ? other : g;
    for (const int i : {1, 2}) {
        // Inward means +y: row 0 is the bottom border.
        CHECK(movedGrid.mesh.position(movedGrid.at(i, 0)).y == doctest::Approx(0.25f));
    }
    // The outward call leaves the whole mesh bit-identical.
    const auto& stillBefore = plusMoved ? beforeOther : beforeG;
    for (Index v = 0; v < stillGrid.mesh.vertexCapacity(); ++v) {
        CHECK(bitIdentical(stillGrid.mesh.position(VertexId{v}), stillBefore[v]));
    }
}

TEST_CASE("a slide by a full rail or more is refused") {
    Grid g = grid(4, 4);
    const EdgeId seed = g.mesh.edgeBetween(g.at(1, 2), g.at(2, 2));
    const auto before = snapshot(g.mesh);
    // At |t| == 1 the loop lands on its neighbour and every rail collapses.
    CHECK(retopo::slideLoop(g.mesh, seed, 1.0f).moved == 0);
    CHECK(retopo::slideLoop(g.mesh, seed, -1.5f).moved == 0);
    CHECK(retopo::slideLoop(g.mesh, seed, std::nanf("")).moved == 0);
    for (Index v = 0; v < g.mesh.vertexCapacity(); ++v) {
        CHECK(bitIdentical(g.mesh.position(VertexId{v}), before[v]));
    }
}

// ---- Region relax ------------------------------------------------------------

TEST_CASE("region relax leaves everything outside the region bit-identical") {
    Grid g = grid(10, 10);
    // Disturb a vertex in the middle so relax has work to do.
    const VertexId centre = g.at(5, 5);
    g.mesh.setPosition(centre, Vec3{5.4f, 5.3f, 0.0f});
    const auto before = snapshot(g.mesh);

    retopo::RelaxParams params;
    params.iterations = 3;
    params.autoPinCorners = false;
    const std::array<VertexId, 1> seeds{centre};
    const retopo::ResnapReport r = retopo::relaxRegion(g.mesh, seeds, 1, params);

    CHECK(r.moved > 0);
    CHECK(g.mesh.position(centre).x != before[centre.value].x);  // the seed did move
    // Anything more than one ring away -- Chebyshev distance > 1 on this grid
    // counts as more than one EDGE hop only when not orthogonal, so use the
    // strict test: any vertex at grid distance >= 2 in either axis.
    for (int j = 0; j <= 10; ++j) {
        for (int i = 0; i <= 10; ++i) {
            if (std::abs(i - 5) + std::abs(j - 5) <= 1) {
                continue;  // the seed and its one-ring
            }
            const VertexId v = g.at(i, j);
            CHECK(bitIdentical(g.mesh.position(v), before[v.value]));
        }
    }
}

TEST_CASE("region relax evens out the spacing it was asked to fix") {
    Grid g = grid(8, 8);
    const VertexId centre = g.at(4, 4);
    g.mesh.setPosition(centre, Vec3{4.45f, 4.35f, 0.0f});

    retopo::RelaxParams params;
    params.iterations = 8;
    params.strength = 0.5f;
    params.autoPinCorners = false;
    const std::array<VertexId, 1> seeds{centre};
    (void)retopo::relaxRegion(g.mesh, seeds, 2, params);

    // A disturbed vertex in a regular grid relaxes back toward its grid point.
    const Vec3 p = g.mesh.position(centre);
    const float error = std::hypot(p.x - 4.0f, p.y - 4.0f);
    CHECK(error < 0.1f);  // it started ~0.57 away
}

TEST_CASE("region relax REACHES exactly `rings` hops, and no further") {
    // The other region tests only prove things stay put, which a relax that
    // never left its seeds would satisfy perfectly -- and such a relax is
    // useless as Auto Relax, whose whole job is "the new AND neighbouring
    // vertices". So: disturb a vertex exactly two hops from the seed. With
    // rings = 2 it must move; with rings = 1 it must stay bit-identical. That
    // pins the boundary from both sides.
    const auto run = [](int rings) {
        Grid g = grid(10, 10);
        const VertexId seed = g.at(5, 5);
        const VertexId twoHops = g.at(7, 5);  // two edges along the row
        g.mesh.setPosition(twoHops, Vec3{7.35f, 5.3f, 0.0f});
        const Vec3 disturbed = g.mesh.position(twoHops);

        retopo::RelaxParams params;
        params.iterations = 4;
        params.autoPinCorners = false;
        const std::array<VertexId, 1> seeds{seed};
        (void)retopo::relaxRegion(g.mesh, seeds, rings, params);
        return std::pair{g.mesh.position(twoHops), disturbed};
    };

    const auto [inside, disturbedA] = run(2);
    CHECK_FALSE(bitIdentical(inside, disturbedA));  // reached: it moved
    // ...and it moved TOWARD its grid point, not merely somewhere.
    CHECK(std::hypot(inside.x - 7.0f, inside.y - 5.0f) <
          std::hypot(disturbedA.x - 7.0f, disturbedA.y - 5.0f));

    const auto [outside, disturbedB] = run(1);
    CHECK(bitIdentical(outside, disturbedB));  // one ring short: untouched
}

TEST_CASE("region relax is TOPOLOGICAL: a nearby but unconnected sheet does not move") {
    // Two parallel sheets 0.05 apart in Z — spatially adjacent, topologically
    // unconnected. A spatial brush around the edit would drag the second sheet;
    // a ring neighbourhood cannot reach it. This is the design claim, tested.
    Grid top = grid(6, 6);
    const Index offset = static_cast<Index>(top.mesh.vertexCapacity());
    Mesh& m = top.mesh;
    for (int j = 0; j <= 6; ++j) {
        for (int i = 0; i <= 6; ++i) {
            m.addVertex(Vec3{static_cast<float>(i), static_cast<float>(j), -0.05f});
        }
    }
    const auto under = [&](int i, int j) {
        return VertexId{offset + static_cast<Index>(j * 7 + i)};
    };
    for (int j = 0; j < 6; ++j) {
        for (int i = 0; i < 6; ++i) {
            m.addFace(std::array<VertexId, 4>{under(i, j), under(i + 1, j), under(i + 1, j + 1),
                                              under(i, j + 1)});
        }
    }
    m.setPosition(top.at(3, 3), Vec3{3.3f, 3.2f, 0.0f});
    // Disturb the lower sheet directly beneath too, so a relax WOULD move it.
    m.setPosition(under(3, 3), Vec3{3.3f, 3.2f, -0.05f});
    const auto before = snapshot(m);

    retopo::RelaxParams params;
    params.iterations = 4;
    params.autoPinCorners = false;
    const std::array<VertexId, 1> seeds{top.at(3, 3)};
    (void)retopo::relaxRegion(m, seeds, 3, params);

    CHECK_FALSE(bitIdentical(m.position(top.at(3, 3)), before[top.at(3, 3).value]));
    for (int j = 0; j <= 6; ++j) {
        for (int i = 0; i <= 6; ++i) {
            const VertexId v = under(i, j);
            CHECK(bitIdentical(m.position(v), before[v.value]));
        }
    }
}

TEST_CASE("region relax honours pins inside the region") {
    Grid g = grid(6, 6);
    const VertexId centre = g.at(3, 3);
    const VertexId pinned = g.at(3, 4);
    g.mesh.setPosition(centre, Vec3{3.3f, 3.3f, 0.0f});
    g.mesh.setPosition(pinned, Vec3{3.2f, 4.2f, 0.0f});
    const Vec3 pinnedAt = g.mesh.position(pinned);

    retopo::PinSet pins;
    pins.pin(pinned);
    retopo::RelaxParams params;
    params.iterations = 4;
    params.autoPinCorners = false;
    const std::array<VertexId, 1> seeds{centre};
    (void)retopo::relaxRegion(g.mesh, seeds, 2, params, &pins);

    CHECK(bitIdentical(g.mesh.position(pinned), pinnedAt));
}

TEST_CASE("region relax with no live seed or negative rings changes nothing") {
    Grid g = grid(4, 4);
    g.mesh.setPosition(g.at(2, 2), Vec3{2.3f, 2.3f, 0.0f});
    const auto before = snapshot(g.mesh);
    retopo::RelaxParams params;
    params.autoPinCorners = false;

    const std::array<VertexId, 0> none{};
    CHECK(retopo::relaxRegion(g.mesh, none, 3, params).moved == 0);
    const std::array<VertexId, 1> seed{g.at(2, 2)};
    CHECK(retopo::relaxRegion(g.mesh, seed, -1, params).moved == 0);
    for (Index v = 0; v < g.mesh.vertexCapacity(); ++v) {
        CHECK(bitIdentical(g.mesh.position(VertexId{v}), before[v]));
    }
}
