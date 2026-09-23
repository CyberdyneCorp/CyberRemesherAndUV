import CCyberRemesher
import Foundation
import XCTest

@testable import CyberRemesher

/// Regioned baking driven from Swift (surface-baking, "Regioned baking with a
/// bounded working set"; engine-bindings, "Regioned baking is reachable from
/// the bindings").
///
/// A binding that marshalled the row pointer, the row index or the region
/// facts wrongly still returns CYBER_OK, so every assertion checks a property
/// only a correct marshalling produces: the bands, assembled in the order they
/// arrived, equal the whole-image bake's pixels.
final class RegionedBakeTests: XCTestCase {

    private func quarterQuad(name: String, z: Float) throws -> Mesh {
        let obj = """
            v 0 0 \(z)
            v 1 0 \(z)
            v 1 1 \(z + 0.2)
            v 0 1 \(z + 0.2)
            vt 0.1 0.1
            vt 0.6 0.1
            vt 0.6 0.6
            vt 0.1 0.6
            f 1/1 2/2 3/3 4/4
            """
        let path = NSTemporaryDirectory() + "cyber_swift_regions_\(name).obj"
        try obj.write(toFile: path, atomically: true, encoding: .utf8)
        return try Mesh.loadOBJ(path)
    }

    private struct Stop: Error {}

    func testBandsAssembleIntoTheWholeImageBake() throws {
        let low = try quarterQuad(name: "low", z: 0)
        let high = try quarterQuad(name: "high", z: 0.02)
        var params = BakeParameters()
        params.width = 32
        params.height = 32
        params.paddingRadius = 4
        params.cageDistance = 0.2
        XCTAssertEqual(params.maxWorkingSetTexels, 0)
        params.maxWorkingSetTexels = 32 * (4 + 2 * 8)  // 4-row regions, 8-row halo

        for map in [BakeMap.normal, BakeMap.objectPosition, BakeMap.uvDensity] {
            let whole = try low.bake(from: high, map: map, parameters: params).pixels()
            var assembled: [Float] = []
            var starts: [Int] = []
            let report = try low.bakeRegions(from: high, map: map, parameters: params) { band in
                starts.append(band.rowBegin)
                assembled.append(contentsOf: band.pixels)
            }
            XCTAssertEqual(starts, Array(stride(from: 0, to: 32, by: 4)))
            XCTAssertEqual(assembled, whole)
            XCTAssertEqual(report.regions,
                           ImageRegions(count: 8, rows: 4, haloRows: 8, workingSetTexels: 32 * 20))
            XCTAssertEqual(report.width, 32)
            XCTAssertTrue(report.pixels().isEmpty)
            XCTAssertEqual(report.padding.radius, 4)
        }
    }

    func testAThrowingRowHandlerStopsTheBakeAndIsRethrown() throws {
        let low = try quarterQuad(name: "stop_low", z: 0)
        let high = try quarterQuad(name: "stop_high", z: 0.02)
        var params = BakeParameters()
        params.width = 32
        params.height = 32
        params.maxWorkingSetTexels = 32 * 20
        var bands = 0
        XCTAssertThrowsError(
            try low.bakeRegions(from: high, map: .normal, parameters: params) { _ in
                bands += 1
                if bands == 2 { throw Stop() }
            }
        ) { error in
            XCTAssertTrue(error is Stop)
        }
        XCTAssertEqual(bands, 2)
    }
}
