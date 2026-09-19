import CCyberRemesher
import Foundation
import XCTest

@testable import CyberRemesher

/// The whole retopology workflow through ONE binding: snap, draw, unwrap, bake.
///
/// Until this change no binding could run it. Swift could draw and not finish;
/// Python could finish and not draw. This test is the proof that the Swift half
/// is now complete — a host can go from a sculpt to a baked map without
/// dropping to C.
///
/// Every assertion checks a property of the RESULT. A binding that marshals an
/// argument wrongly usually still returns CYBER_OK, so "did not throw" proves
/// very little.
final class RetopologyWorkflowTests: XCTestCase {

    /// A triangulated tube about +Y, open at both ends.
    private func cylinder(radius: Float = 1, around: Int = 48, along: Int = 24) throws -> Mesh {
        var positions: [Float] = []
        for j in 0...along {
            let y = -1 + 2 * Float(j) / Float(along)
            for i in 0..<around {
                let a = 2 * Float.pi * Float(i) / Float(around)
                positions += [radius * cos(a), y, radius * sin(a)]
            }
        }
        var indices: [UInt32] = []
        for j in 0..<along {
            for i in 0..<around {
                let i1 = (i + 1) % around
                let a = UInt32(j * around + i)
                let b = UInt32(j * around + i1)
                let c = UInt32((j + 1) * around + i1)
                let d = UInt32((j + 1) * around + i)
                indices += [a, b, c, a, c, d]
            }
        }
        return try Mesh(positions: positions, indices: indices)
    }

    /// Stroke samples covering only the NEAR side of the tube at height `y`.
    private func arc(_ y: Float, count: Int = 12, sweep: Float = 2.2) -> [SIMD3<Float>] {
        (0..<count).map { k in
            let a = -sweep / 2 + sweep * Float(k) / Float(count - 1)
            return SIMD3(cos(a), y, sin(a))
        }
    }

    func testSnapperRaycastHitsTheNearSide() throws {
        let target = try cylinder()
        let snapper = try Snapper(target: target)

        // Off-axis on purpose: a ray straight down the axis lands on the same
        // point with origin and direction transposed, so it cannot tell a
        // correct binding from a swapped one.
        let hit = try XCTUnwrap(
            snapper.raycast(origin: SIMD3(0.3, 0.2, 5), direction: SIMD3(0, 0, -1)))
        let nearZ = (1 - 0.3 * 0.3 as Float).squareRoot()
        XCTAssertEqual(hit.point.x, 0.3, accuracy: 2e-3)
        XCTAssertEqual(hit.point.y, 0.2, accuracy: 2e-3)
        XCTAssertEqual(hit.point.z, nearZ, accuracy: 5e-3)
        XCTAssertEqual(hit.distance, 5 - nearZ, accuracy: 5e-3)

        XCTAssertNil(
            snapper.raycast(origin: SIMD3(0, 0, 3), direction: SIMD3(0, 0, 1)),
            "a ray pointing away must miss, or a tap on empty space would place a vertex")
    }

    func testContoursBuildAClosedTubeOnTheTarget() throws {
        let target = try cylinder()
        let snapper = try Snapper(target: target)
        let edit = try Mesh()

        let report = try edit.contours(
            target: target, strokes: [arc(-0.6), arc(0), arc(0.6)], spans: 12, snapper: snapper)

        XCTAssertEqual(report.ringCount, 3)
        XCTAssertEqual(report.vertexCount, 36)
        XCTAssertEqual(report.faceCount, 24)

        // Every ring vertex is ON the Target, including the far side the
        // strokes never covered.
        let positions = try edit.positions()
        var minAngle = Float.pi
        var maxAngle = -Float.pi
        for i in 0..<edit.vertexCount {
            let x = positions[3 * i]
            let z = positions[3 * i + 2]
            XCTAssertEqual((x * x + z * z).squareRoot(), 1, accuracy: 1e-3)
            if i < 12 {
                let a = atan2(z, x)
                minAngle = min(minAngle, a)
                maxAngle = max(maxAngle, a)
            }
        }
        XCTAssertGreaterThan(maxAngle - minAngle, 4, "the ring must wrap past the 2.2 rad stroke")
    }

    func testContoursRefuseAStrokeThatNamesNoPlane() throws {
        let target = try cylinder()
        let edit = try Mesh()
        let straight = (0..<10).map { k in SIMD3<Float>(1, -0.5 + 0.1 * Float(k), 0) }

        XCTAssertThrowsError(
            try edit.contours(target: target, strokes: [arc(0), straight], spans: 8))
        XCTAssertEqual(edit.faceCount, 0, "a refused run must leave the mesh untouched")
    }

    func testUnwrapThenBakeFinishesTheAsset() throws {
        let target = try cylinder()
        let snapper = try Snapper(target: target)
        let low = try Mesh()
        try low.contours(
            target: target, strokes: [arc(-0.6), arc(-0.2), arc(0.2), arc(0.6)], spans: 16,
            snapper: snapper)
        XCTAssertEqual(low.faceCount, 48)

        // UV: a tube needs at least one cut to lie flat, so zero seams would
        // mean the unwrap did not actually run.
        let atlas = try low.unwrap()
        XCTAssertGreaterThanOrEqual(atlas.chartCount, 1)
        XCTAssertGreaterThan(atlas.seamEdges, 0)
        XCTAssertEqual(atlas.droppedCharts, 0, "a dropped chart covers no texels")
        XCTAssertEqual(atlas.flippedCharts, 0, "a flipped chart bakes inside-out")
        XCTAssertGreaterThan(atlas.packedArea, 0)
        XCTAssertLessThanOrEqual(atlas.packedArea, atlas.packedBoxArea + 1e-4)

        // Bake: the low mesh sits exactly on the high one, so a normal map
        // must come back the requested size and carry real, not blank, data.
        var params = BakeParameters()
        params.width = 64
        params.height = 64
        let image = try low.bake(from: target, map: .normal, parameters: params)
        XCTAssertEqual(image.width, 64)
        XCTAssertEqual(image.height, 64)
        XCTAssertEqual(image.channels, 3)

        let pixels = image.pixels()
        XCTAssertEqual(pixels.count, 64 * 64 * 3)
        XCTAssertFalse(pixels.contains { !$0.isFinite }, "a baked map must be finite everywhere")
        XCTAssertTrue(pixels.contains { $0 != pixels[0] }, "a map of one colour was not baked")

        let url = FileManager.default.temporaryDirectory
            .appendingPathComponent("retopo-workflow-\(UUID().uuidString).png")
        defer { try? FileManager.default.removeItem(at: url) }
        try image.savePNG(to: url.path)
        let size = try FileManager.default.attributesOfItem(atPath: url.path)[.size] as? Int
        XCTAssertGreaterThan(size ?? 0, 0)
    }

    func testAtlasParametersReachTheEngine() throws {
        // `textureSize` only drives the texel-density readout, so doubling it
        // must double `texelDensity` on the same mesh and change nothing else.
        //
        // This exists because the unwrap test above uses defaults, and the C
        // ABI treats a NULL params pointer as "use defaults" — so a binding that
        // dropped the parameters entirely passed it. Only a NON-default value
        // can prove the struct actually crossed the boundary.
        func tube() throws -> Mesh {
            let target = try cylinder()
            let snapper = try Snapper(target: target)
            let low = try Mesh()
            try low.contours(
                target: target, strokes: [arc(-0.5), arc(0), arc(0.5)], spans: 12,
                snapper: snapper)
            return low
        }

        var small = AtlasParameters()
        small.textureSize = 512
        var large = AtlasParameters()
        large.textureSize = 1024

        let a = try tube().unwrap(small)
        let b = try tube().unwrap(large)
        XCTAssertGreaterThan(a.texelDensity, 0)
        XCTAssertEqual(b.texelDensity / a.texelDensity, 2, accuracy: 1e-3)
        XCTAssertEqual(a.chartCount, b.chartCount, "texture size must not change the charts")
        XCTAssertEqual(a.seamEdges, b.seamEdges)
    }

    func testMeshMapsCarryADecodableEncodingBasis() throws {
        // The four CyberTexel maps through the Swift surface. An object-space
        // position map is `(p - min) / (max - min)`: without the recorded box a
        // consumer cannot recover a single coordinate, so the binding has to
        // carry the basis and not only the pixels.
        let target = try cylinder()
        let snapper = try Snapper(target: target)
        let low = try Mesh()
        try low.contours(
            target: target, strokes: [arc(-0.5), arc(0), arc(0.5)], spans: 12, snapper: snapper)
        _ = try low.unwrap()

        var params = BakeParameters()
        params.width = 32
        params.height = 32
        params.aoSamples = 8

        let objectNormal = try low.bake(from: target, map: .objectNormal, parameters: params)
        XCTAssertEqual(objectNormal.channels, 3)
        XCTAssertEqual(objectNormal.encoding.basis, .objectNormal)
        XCTAssertEqual(objectNormal.encoding.upAxis, .y)

        let position = try low.bake(from: target, map: .objectPosition, parameters: params)
        XCTAssertEqual(position.encoding.basis, .objectBounds)
        XCTAssertGreaterThan(position.encoding.boundsMax.0, position.encoding.boundsMin.0)
        XCTAssertFalse(position.pixels().contains { $0 < 0 || $0 > 1 },
                       "an object-space position must land inside its own bounds")

        let bent = try low.bake(from: target, map: .bentNormal, parameters: params)
        XCTAssertEqual(bent.encoding.basis, .tangentNormal)

        // A non-default value is the only thing that proves the appended
        // members crossed the boundary at all.
        params.bentNormalSpace = .object
        params.upAxis = .z
        let bentObject = try low.bake(from: target, map: .bentNormal, parameters: params)
        XCTAssertEqual(bentObject.encoding.basis, .objectNormal)
        XCTAssertEqual(bentObject.encoding.upAxis, .z)

        params.thicknessScale = 3
        let thickness = try low.bake(from: target, map: .thickness, parameters: params)
        XCTAssertEqual(thickness.channels, 1)
        XCTAssertEqual(thickness.encoding.basis, .distance)
        XCTAssertEqual(thickness.encoding.scale, 3, accuracy: 1e-6)

        // Out of range is refused, not defaulted. `upAxis` is a Swift enum, so
        // the C ABI's own validation is reached through the scale instead.
        params.thicknessScale = -1
        XCTAssertThrowsError(try low.bake(from: target, map: .thickness, parameters: params))
    }

    func testIdMapsCarryATableThatResolvesTheirColours() throws {
        // A colour-ID map is useless without the mapping that turns a picked
        // colour back into a material or object, so the two are checked
        // together: every non-padding texel must be a colour the reported
        // table names, at zero tolerance.
        let target = try cylinder()
        let snapper = try Snapper(target: target)
        let low = try Mesh()
        try low.contours(
            target: target, strokes: [arc(-0.5), arc(0), arc(0.5)], spans: 12, snapper: snapper)
        _ = try low.unwrap()

        var params = BakeParameters()
        params.width = 32
        params.height = 32

        let objectId = try low.bake(from: target, map: .objectId, parameters: params)
        XCTAssertEqual(objectId.channels, 3)
        XCTAssertEqual(objectId.encoding.basis, .idColor)
        // No id column is declared on a contoured Target, so the object map
        // falls back to face-connected components and reports that.
        XCTAssertEqual(objectId.encoding.idSource, "component")
        XCTAssertFalse(objectId.encoding.idColors.isEmpty)
        for row in objectId.encoding.idColors {
            XCTAssertFalse(row.color == (0, 0, 0), "black is reserved for 'no id'")
            XCTAssertGreaterThanOrEqual(min(row.color.0, min(row.color.1, row.color.2)), 64)
        }

        // A consumer resolves a picked colour by building the row it is
        // looking for and comparing, so the row type has to be constructible
        // from host code and compare by value.
        let row = try XCTUnwrap(objectId.encoding.idColors.first)
        XCTAssertEqual(row, IdColor(id: row.id, color: row.color))
        XCTAssertNotEqual(row, IdColor(id: row.id &+ 1, color: row.color))

        let table = Set(objectId.encoding.idColors.map {
            [Int($0.color.0), Int($0.color.1), Int($0.color.2)]
        })
        let pixels = objectId.pixels()
        for texel in stride(from: 0, to: pixels.count - 2, by: 3) {
            let rgb = (0..<3).map { Int((pixels[texel + $0] * 255).rounded()) }
            XCTAssertTrue(rgb == [0, 0, 0] || table.contains(rgb),
                          "texel \(rgb) resolves to no row in the reported table")
        }

        // Every other map reports no table at all.
        let normal = try low.bake(from: target, map: .normal, parameters: params)
        XCTAssertEqual(normal.encoding.idSource, "")
        XCTAssertTrue(normal.encoding.idColors.isEmpty)
    }

    func testDefaultsComeFromTheEngine() {
        // The Swift mirrors read their defaults through the C ABI rather than
        // repeating them, so they cannot drift from what the CLI does.
        var atlas = CyberAtlasParams()
        cyber_default_atlas_params(&atlas)
        let swiftAtlas = AtlasParameters()
        XCTAssertEqual(swiftAtlas.packMargin, atlas.packMargin)
        XCTAssertEqual(swiftAtlas.textureSize, atlas.textureSize)

        var bake = CyberBakeParams()
        cyber_default_bake_params(&bake)
        let swiftBake = BakeParameters()
        XCTAssertEqual(swiftBake.width, bake.width)
        XCTAssertEqual(swiftBake.cageDistance, bake.cageDistance)
        XCTAssertEqual(swiftBake.thicknessScale, bake.thicknessScale)
        XCTAssertEqual(Int32(bitPattern: swiftBake.upAxis.rawValue), bake.upAxis)
        XCTAssertEqual(
            Int32(bitPattern: swiftBake.bentNormalSpace.rawValue), bake.bentNormalSpace)
    }
}
