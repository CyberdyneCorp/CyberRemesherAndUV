import CCyberRemesher
import Foundation
import XCTest

@testable import CyberRemesher

/// Region relax, loop slide and interactive symmetry through Swift.
///
/// Fixtures are chosen so a dropped or swapped field changes the answer: a
/// negated `t` must slide the other way, a flipped working side must mirror
/// nothing, a region one ring too small must leave a two-hop vertex in place.
final class RetopoFeelTests: XCTestCase {

    private func grid(_ nx: Int, _ ny: Int) throws -> (Mesh, (Int, Int) -> UInt32) {
        var positions: [Float] = []
        for j in 0...ny { for i in 0...nx { positions += [Float(i), Float(j), 0] } }
        let at = { (i: Int, j: Int) in UInt32(j * (nx + 1) + i) }
        var indices: [UInt32] = []
        for j in 0..<ny {
            for i in 0..<nx { indices += [at(i, j), at(i + 1, j), at(i + 1, j + 1), at(i, j + 1)] }
        }
        let offsets = stride(from: 0, through: indices.count, by: 4).map { $0 }
        return (try Mesh(positions: positions, faceOffsets: offsets, indices: indices), at)
    }

    private func move(_ mesh: Mesh, _ vertex: UInt32, to p: SIMD3<Float>) throws {
        var flat = mesh.positions()
        flat[Int(vertex) * 3] = p.x
        flat[Int(vertex) * 3 + 1] = p.y
        flat[Int(vertex) * 3 + 2] = p.z
        try mesh.setPositions(flat)
    }

    private func edge(_ mesh: Mesh, _ a: UInt32, _ b: UInt32) throws -> UInt32 {
        let pa = try XCTUnwrap(mesh.vertexPosition(a))
        let pb = try XCTUnwrap(mesh.vertexPosition(b))
        return try XCTUnwrap(mesh.nearestEdge(to: (pa + pb) / 2, maxDistance: 0.1))
    }

    func testSlideSignSelectsTheSide() throws {
        let (up, at) = try grid(5, 5)
        let (down, _) = try grid(5, 5)
        let r = try up.slideLoop(edge: edge(up, at(1, 2), at(2, 2)), t: 0.4)
        XCTAssertEqual(r.loopVertices, 6)
        XCTAssertEqual(r.moved, 6)
        try down.slideLoop(edge: edge(down, at(1, 2), at(2, 2)), t: -0.4)

        let dyUp = try XCTUnwrap(up.vertexPosition(at(0, 2))).y - 2
        let dyDown = try XCTUnwrap(down.vertexPosition(at(0, 2))).y - 2
        XCTAssertEqual(abs(dyUp), 0.4, accuracy: 1e-5)
        XCTAssertEqual(dyDown, -dyUp, accuracy: 1e-5, "t and -t must be mirror images")
        for i in 0...5 {
            XCTAssertEqual(try XCTUnwrap(up.vertexPosition(at(i, 2))).y, 2 + dyUp, accuracy: 1e-5)
        }
        XCTAssertThrowsError(try up.slideLoop(edge: edge(up, at(1, 2), at(2, 2)), t: 1))
    }

    func testRegionRelaxReachesExactlyItsRings() throws {
        func run(_ rings: Int) throws -> (SIMD3<Float>, SIMD3<Float>) {
            let (mesh, at) = try grid(10, 10)
            try move(mesh, at(7, 5), to: SIMD3(7.35, 5.3, 0))
            let disturbed = try XCTUnwrap(mesh.vertexPosition(at(7, 5)))
            try mesh.relaxRegion(
                seeds: [at(5, 5)], rings: rings, iterations: 4, autoPinCorners: false)
            return (try XCTUnwrap(mesh.vertexPosition(at(7, 5))), disturbed)
        }
        let (inside, before) = try run(2)
        XCTAssertNotEqual(inside, before, "a two-hop vertex must move when rings = 2")
        let (outside, before1) = try run(1)
        XCTAssertEqual(outside, before1, "one ring short must leave it exactly in place")
    }

    func testApplySymmetryHonoursTheWorkingSide() throws {
        let (positive, _) = try grid(3, 2)
        let added = try positive.applySymmetry(
            MirrorPlane(normal: SIMD3(1, 0, 0), weldTolerance: 1e-3, workingSidePositive: true))
        XCTAssertEqual(added, 6)
        XCTAssertEqual(positive.faceCount, 12)
        XCTAssertEqual(positive.vertexCount, 21, "the x = 0 seam must be shared, not duplicated")

        let (negative, _) = try grid(3, 2)
        let none = try negative.applySymmetry(
            MirrorPlane(normal: SIMD3(1, 0, 0), weldTolerance: 1e-3, workingSidePositive: false))
        XCTAssertEqual(none, 0, "nothing lies on the -x side to mirror")
    }
}
