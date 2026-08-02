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
