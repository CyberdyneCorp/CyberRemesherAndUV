#include <doctest.h>

#include <array>
#include <cstddef>
#include <vector>

#include "../../src/quadrangulate/src/bimdf_quantize.hpp"

using cyber::remesh::bimdf::Arc;
using cyber::remesh::bimdf::BimdfResult;
using cyber::remesh::bimdf::Patch;
using cyber::remesh::bimdf::solveBimdf;
using cyber::remesh::bimdf::TMesh;

namespace {

// The textbook cube T-mesh: 8 corner nodes, 12 arcs, 6 quad patches (the
// exact structure box_sharp traces). Arc order: x-edges 0..3, y-edges 4..7,
// z-edges 8..11. Consistency forces every parallel class to one value.
TMesh cubeTMesh(double tx, double ty, double tz) {
    TMesh tm;
    tm.ok = true;
    tm.nodeCount = 8;
    // Corners: bit 0 = x, bit 1 = y, bit 2 = z.
    const auto addArc = [&](std::size_t a, std::size_t b, double len) {
        Arc arc;
        arc.n0 = a;
        arc.n1 = b;
        arc.len = len;
        tm.arcs.push_back(arc);
    };
    // x-edges (vary bit 0): (y,z) in {0,1}^2.
    addArc(0, 1, tx);  // y0 z0
    addArc(2, 3, tx);  // y1 z0
    addArc(4, 5, tx);  // y0 z1
    addArc(6, 7, tx);  // y1 z1
    // y-edges (vary bit 1).
    addArc(0, 2, ty);
    addArc(1, 3, ty);
    addArc(4, 6, ty);
    addArc(5, 7, ty);
    // z-edges (vary bit 2).
    addArc(0, 4, tz);
    addArc(1, 5, tz);
    addArc(2, 6, tz);
    addArc(3, 7, tz);
    const auto quad = [&](std::size_t s0, std::size_t s1, std::size_t s2, std::size_t s3) {
        Patch p;
        p.side[0] = {s0};
        p.side[1] = {s1};
        p.side[2] = {s2};
        p.side[3] = {s3};
        tm.patches.push_back(p);
    };
    quad(0, 5, 1, 4);    // z = 0 face: x, y, x, y
    quad(2, 7, 3, 6);    // z = 1 face
    quad(0, 9, 2, 8);    // y = 0 face: x, z, x, z
    quad(1, 11, 3, 10);  // y = 1 face
    quad(4, 10, 6, 8);   // x = 0 face: y, z, y, z
    quad(5, 11, 7, 9);   // x = 1 face
    return tm;
}

long long classValue(const BimdfResult& r, std::size_t first) { return r.arcLenHalf[first]; }

}  // namespace

TEST_CASE("bimdf cube quantizes each parallel class to the rounded target") {
    const TMesh tm = cubeTMesh(4.2, 3.7, 5.5);
    const BimdfResult r = solveBimdf(tm);
    REQUIRE(r.ok);
    CHECK(r.maxSideViolation == 0);
    // Consistency: all four arcs of a class equal.
    for (std::size_t c = 0; c < 3; ++c) {
        for (std::size_t k = 1; k < 4; ++k) {
            CHECK(r.arcLenHalf[4 * c + k] == r.arcLenHalf[4 * c]);
        }
    }
    // Targets in half-cells: 8.4 -> 8, 7.4 -> 7, 11 -> 11.
    CHECK(classValue(r, 0) == 8);
    CHECK(classValue(r, 4) == 7);
    CHECK(classValue(r, 8) == 11);
}

TEST_CASE("bimdf T-node split side stays consistent with its opposite side") {
    TMesh tm = cubeTMesh(4.2, 3.7, 2.35);
    // Split one z-edge (arc 8: nodes 0-4) at a T-node (node 8):
    // 2.35 = 1.1 + 1.25. Naive per-arc rounding gives 2 + 3 = 5 half-cells
    // against the opposite side's round(4.7) = 5 — the solver must return
    // side sums that match exactly.
    tm.arcs[8].n1 = 8;
    tm.arcs[8].len = 1.1;
    Arc tail;
    tail.n0 = 8;
    tail.n1 = 4;
    tail.len = 1.25;
    tm.arcs.push_back(tail);  // arc 12
    tm.nodeCount = 9;
    for (Patch& p : tm.patches) {
        for (auto& side : p.side) {
            if (side.size() == 1 && side[0] == 8) {
                side = {8, 12};
            }
        }
    }
    const BimdfResult r = solveBimdf(tm);
    REQUIRE(r.ok);
    CHECK(r.maxSideViolation == 0);
    CHECK(r.arcLenHalf[8] + r.arcLenHalf[12] == r.arcLenHalf[9]);
    CHECK(r.arcLenHalf[9] == r.arcLenHalf[10]);
    CHECK(r.arcLenHalf[9] == r.arcLenHalf[11]);
}

TEST_CASE("bimdf even-sum template quantizes a hexagonal patch beside quads") {
    // Cube with the z=0 and y=0 faces merged into one hexagonal patch (their
    // shared x-edge removed): 11 arcs, 4 quads, 1 hexagon. The quad
    // constraints still tie each parallel class to one value; the hexagon
    // must quantize through the interior-routing template with an even
    // boundary sum.
    TMesh tm;
    tm.ok = true;
    tm.nodeCount = 8;
    const auto addArc = [&](std::size_t a, std::size_t b, double len) {
        Arc arc;
        arc.n0 = a;
        arc.n1 = b;
        arc.len = len;
        tm.arcs.push_back(arc);
    };
    const double tx = 4.2, ty = 3.7, tz = 5.5;
    addArc(2, 3, tx);  // 0: X1
    addArc(4, 5, tx);  // 1: X2
    addArc(6, 7, tx);  // 2: X3
    addArc(0, 2, ty);  // 3: Y0
    addArc(1, 3, ty);  // 4: Y1
    addArc(4, 6, ty);  // 5: Y2
    addArc(5, 7, ty);  // 6: Y3
    addArc(0, 4, tz);  // 7: Z0
    addArc(1, 5, tz);  // 8: Z1
    addArc(2, 6, tz);  // 9: Z2
    addArc(3, 7, tz);  // 10: Z3
    const auto quad = [&](std::size_t s0, std::size_t s1, std::size_t s2, std::size_t s3) {
        Patch p;
        p.side[0] = {s0};
        p.side[1] = {s1};
        p.side[2] = {s2};
        p.side[3] = {s3};
        tm.patches.push_back(p);
    };
    quad(1, 6, 2, 5);   // z = 1
    quad(0, 10, 2, 9);  // y = 1
    quad(3, 9, 5, 7);   // x = 0
    quad(4, 10, 6, 8);  // x = 1
    Patch hex;
    hex.side = {{4}, {0}, {3}, {7}, {1}, {8}};  // Y1, X1, Y0, Z0, X2, Z1
    tm.patches.push_back(hex);
    const BimdfResult r = solveBimdf(tm);
    REQUIRE(r.ok);
    CHECK(r.polyPatches == 1);
    CHECK(r.polyOddSum == 0);
    CHECK(r.maxSideViolation == 0);
    // Parallel classes still collapse to one value each.
    CHECK(r.arcLenHalf[1] == r.arcLenHalf[0]);
    CHECK(r.arcLenHalf[2] == r.arcLenHalf[0]);
    for (std::size_t k = 4; k <= 6; ++k) {
        CHECK(r.arcLenHalf[k] == r.arcLenHalf[3]);
    }
    for (std::size_t k = 8; k <= 10; ++k) {
        CHECK(r.arcLenHalf[k] == r.arcLenHalf[7]);
    }
    long long boundary = 0;
    for (const auto& side : hex.side) {
        for (const std::size_t a : side) {
            boundary += r.arcLenHalf[a];
        }
    }
    CHECK(boundary % 2 == 0);
}

TEST_CASE("bimdf even-sum template fixes an odd polygonal boundary by parity flips") {
    // Two triangles glued along all three arcs (a pillow): every side sum
    // rounds to 3, so each patch's boundary guess is 9 — odd. The T-join
    // parity sweep must adjust some arc so both boundaries come out even,
    // without violating the min-one guard.
    TMesh tm;
    tm.ok = true;
    tm.nodeCount = 3;
    for (int k = 0; k < 3; ++k) {
        Arc arc;
        arc.n0 = static_cast<std::size_t>(k);
        arc.n1 = static_cast<std::size_t>((k + 1) % 3);
        arc.len = 1.6;
        tm.arcs.push_back(arc);
    }
    for (int p = 0; p < 2; ++p) {
        Patch tri;
        tri.side = {{0}, {1}, {2}};
        tm.patches.push_back(tri);
    }
    const BimdfResult r = solveBimdf(tm);
    REQUIRE(r.ok);
    CHECK(r.polyPatches == 2);
    CHECK(r.polyOddSum == 0);
    long long boundary = 0;
    for (const long long x : r.arcLenHalf) {
        CHECK(x >= 1);
        boundary += x;
    }
    CHECK(boundary % 2 == 0);
    CHECK(r.parityFlips > 0);
}

TEST_CASE("bimdf enforces the min-one collapse guard on degenerate arcs") {
    // A near-zero z class would collapse the cube; the guard must keep every
    // arc at >= 1 half-cell and report the adjustment.
    const TMesh tm = cubeTMesh(4.0, 3.0, 0.05);
    const BimdfResult r = solveBimdf(tm);
    REQUIRE(r.ok);
    CHECK(r.maxSideViolation == 0);
    for (const long long x : r.arcLenHalf) {
        CHECK(x >= 1);
    }
    CHECK(r.raisedToMin > 0);
    CHECK(classValue(r, 8) == 1);
}

// --- feasible-rotation projection (ZRemesher Phase B) ----------------------
//
// The sector quarters around a layout node are what a rotation system needs:
// the wrap gap in [1, wrapMax] and every other sector a corner (1) or a
// pass-through (2). Largest-remainder rounding minimizes per-gap error while
// ignoring that constraint, so it can land out of range even when an in-range
// assignment exists; the projection is what recovers the node in that case.

using cyber::remesh::bimdf::projectSectors;

TEST_CASE("projectSectors seats a feasible winding at the widest gaps") {
    std::vector<int> sect;
    // Four ends, winding 6: two sectors must take the extra quarter, and they
    // must be the two that measured widest.
    const std::vector<double> gq = {1.1, 1.9, 1.05, 1.95};
    REQUIRE(projectSectors(gq, 6, 2, sect));
    REQUIRE(sect.size() == 4);
    CHECK(sect[0] == 1);
    CHECK(sect[1] == 2);
    CHECK(sect[2] == 1);
    CHECK(sect[3] == 2);
    CHECK(sect[0] + sect[1] + sect[2] + sect[3] == 6);
}

TEST_CASE("projectSectors respects the wider wrap bound") {
    std::vector<int> sect;
    // A single end carries the whole winding in its wrap sector.
    const std::vector<double> gq = {4.0};
    REQUIRE(projectSectors(gq, 4, 4, sect));
    REQUIRE(sect.size() == 1);
    CHECK(sect[0] == 4);
    // With the ordinary wrap bound the same winding is unseatable.
    CHECK_FALSE(projectSectors(gq, 4, 2, sect));
}

TEST_CASE("projectSectors refuses an infeasible winding") {
    std::vector<int> sect;
    const std::vector<double> gq = {1.0, 1.0, 1.0};
    // Below the floor: three sectors cannot sum to less than three.
    CHECK_FALSE(projectSectors(gq, 2, 2, sect));
    // Above the ceiling: 2 + 2 + 2 = 6 is the most three sectors can carry.
    CHECK_FALSE(projectSectors(gq, 7, 2, sect));
    // Both bounds are inclusive.
    CHECK(projectSectors(gq, 3, 2, sect));
    CHECK(projectSectors(gq, 6, 2, sect));
}

TEST_CASE("projectSectors is deterministic under ties") {
    // Equal gaps: the surplus must always land on the same sectors, so a
    // layout does not change shape between runs.
    const std::vector<double> gq = {1.5, 1.5, 1.5, 1.5};
    std::vector<int> first;
    REQUIRE(projectSectors(gq, 6, 2, first));
    for (int i = 0; i < 8; ++i) {
        std::vector<int> again;
        REQUIRE(projectSectors(gq, 6, 2, again));
        CHECK(again == first);
    }
}
