import CCyberRemesher
import Foundation
import XCTest

@testable import CyberRemesher

/// The bake provider surface, driven from Swift.
///
/// The engine-bindings spec requires this surface to be reachable "from the
/// Python and Swift bindings on the same terms". Python has
/// `python_test_bake_provider`; until this file the Swift half was only
/// COMPILED, which is the exact gap an earlier change in this repo closed for
/// the rest of the binding: compiling proves the package references symbols
/// that exist, never that the marshalling works. A binding that hands the
/// engine the wrong pointer, drops a parameter or loses a callback box still
/// returns CYBER_OK.
///
/// So every assertion here checks a property of the RESULT, and two of them
/// check a property the binding could only get right by actually marshalling:
/// the provider's pixels against the ordinary bake's, texel for texel, and a
/// parameter (the projection cage) whose value changes what the rays find.
final class BakeProviderTests: XCTestCase {

    /// A unit plane in z = `depth`, two triangles, wound so its normal is +Z.
    private func plane(depth: Float = 0) throws -> Mesh {
        let positions: [Float] = [
            0, 0, depth,
            1, 0, depth,
            1, 1, depth,
            0, 1, depth,
        ]
        return try Mesh(positions: positions, indices: [0, 1, 2, 0, 2, 3])
    }

    /// The low-poly of every case: a plane carrying UVs, which a bake needs.
    private func unwrappedPlane() throws -> Mesh {
        let low = try plane()
        _ = try low.unwrap()
        return low
    }

    private func parameters(size: Int32 = 32, cage: Float? = nil) -> BakeParameters {
        var params = BakeParameters()
        params.width = size
        params.height = size
        if let cage { params.cageDistance = cage }
        return params
    }

    // MARK: - the capability query

    func testTheAdvertisedSetArrivesWholeAndIsReachableByName() throws {
        let maps = BakeProvider.maps
        XCTAssertEqual(maps.count, Int(cyber_bake_provider_map_count()))
        XCTAssertFalse(maps.isEmpty)

        let list = BakeProvider.mapList()
        let fieldList = BakeProvider.mapList(fieldOnly: true)
        XCTAssertFalse(list.isEmpty)
        XCTAssertLessThan(fieldList.count, list.count)

        var names = Set<String>()
        for entry in maps {
            XCTAssertTrue(names.insert(entry.name).inserted, "\(entry.name) advertised twice")
            XCTAssertFalse(entry.name.isEmpty)
            // A host sizes its buffer from this, so a zero would be a crash and
            // a wrong value a buffer overrun.
            XCTAssertTrue(entry.channels == 1 || entry.channels == 3, entry.name)
            XCTAssertTrue(list.contains(entry.name), "\(entry.name) missing from the name list")
            XCTAssertEqual(fieldList.contains(entry.name), entry.fieldCapable, entry.name)

            // Same record by name as by index: the two lookups must not drift.
            let byName = try BakeProvider.map(named: entry.name)
            XCTAssertEqual(byName, entry)
        }

        // A host reads the colour space to decide whether to put a transfer
        // curve on the texels. "srgb" on a normal, a distance or an id key
        // gamma-encodes DATA; "linear" on colour ships a washed-out base map.
        for entry in maps {
            XCTAssertTrue(
                entry.colorSpace == "linear" || entry.colorSpace == "srgb", entry.colorSpace)
            XCTAssertEqual(entry.colorSpace, entry.name == "color" ? "srgb" : "linear", entry.name)
        }
        XCTAssertTrue(maps.contains { $0.colorSpace == "srgb" }, "no map is appearance any more?")

        XCTAssertThrowsError(try BakeProvider.map(named: "no-such-map")) { error in
            // The refusal has to NAME the advertised set, or the host is left
            // guessing what it could have asked for.
            XCTAssertTrue("\(error)".contains("normal"), "\(error)")
        }
    }

    // MARK: - the bake

    func testProviderPixelsMatchTheOrdinaryBakeTexelForTexel() throws {
        // The provider must take the SHARED path. If the Swift binding
        // marshalled a parameter wrongly, or handed the engine a different
        // mesh, this is where it shows: the two calls would disagree.
        let low = try unwrappedPlane()
        let high = try plane()
        let params = parameters()

        let reference = try low.bake(from: high, map: .objectNormal, parameters: params)
        let referencePixels = reference.pixels()

        let output = try low.bakeThroughProvider(
            from: high, map: .objectNormal, parameters: params)

        XCTAssertEqual(output.width, Int(params.width))
        XCTAssertEqual(output.height, Int(params.height))
        XCTAssertEqual(output.channels, reference.channels)
        XCTAssertEqual(output.pixels.count, Int(params.width * params.height) * output.channels)
        XCTAssertEqual(output.pixels.count, referencePixels.count)
        XCTAssertEqual(output.pixels, referencePixels)

        XCTAssertEqual(output.encoding.basis, reference.encoding.basis)
        XCTAssertEqual(output.encoding.upAxis, reference.encoding.upAxis)
        XCTAssertEqual(output.padding.radius, reference.padding.radius)
        XCTAssertEqual(output.padding.mode, reference.padding.mode)
        XCTAssertEqual(output.normalGreenPlusY, 1, "this engine bakes +Y and says so")
        XCTAssertGreaterThan(output.texelsCovered, 0)

        // The advertised channel count is the one the map actually came back
        // with — the number the host allocated from.
        let advertised = try BakeProvider.map(named: "object-normal")
        XCTAssertEqual(advertised.channels, output.channels)
        XCTAssertEqual(advertised.encodingBasis, output.encoding.basis)
    }

    func testTheProjectionCageReachesTheEngineThroughTheBinding() throws {
        // cageDistance is not cosmetic: rays start at surface + normal * cage
        // and a projection is accepted only within twice that, so the cage
        // decides which Target a texel sees at all. A binding that dropped it
        // would still return CYBER_OK and a plausible map.
        //
        // Target sunk 0.3 below the EditMesh. A 0.1 cage searches 0.2 and never
        // reaches it; a 0.5 cage searches 1.0 and does. `.position` writes the
        // hit point on a hit and the EditMesh's own point on a miss.
        let depth: Float = 0.3
        let low = try unwrappedPlane()
        let high = try plane(depth: -depth)

        func depths(cage: Float) throws -> [Float] {
            let output = try low.bakeThroughProvider(
                from: high, map: .position, parameters: parameters(cage: cage))
            XCTAssertEqual(output.channels, 3)
            return stride(from: 2, to: output.pixels.count, by: 3).map { output.pixels[$0] }
        }

        let tooShort = try depths(cage: 0.1)
        let longEnough = try depths(cage: 0.5)

        XCTAssertFalse(
            tooShort.contains { $0 < -depth / 2 },
            "a cage that cannot reach the Target must not report the Target")
        XCTAssertTrue(
            longEnough.contains { abs($0 + depth) < 1e-3 },
            "a cage that reaches the Target must report the Target")
    }

    func testProgressAndCancellationCrossTheBinding() throws {
        let low = try unwrappedPlane()
        let high = try plane()

        var reports: [Float] = []
        let output = try low.bakeThroughProvider(
            from: high, map: .ambientOcclusion, parameters: parameters(),
            progress: { fraction, _ in reports.append(fraction) })
        XCTAssertEqual(output.channels, 1)
        // Repeatedly, not once at the end: a host driving a progress bar off a
        // single 1.0 has no progress bar.
        XCTAssertGreaterThan(reports.count, 1)
        XCTAssertTrue(reports.allSatisfy { $0 >= 0 && $0 <= 1 })

        // The callback box is handed to C as an opaque pointer ARC cannot see.
        // A cancel observed at all is what proves the box survived the call.
        var asked = 0
        XCTAssertThrowsError(
            try low.bakeThroughProvider(
                from: high, map: .ambientOcclusion, parameters: parameters(),
                cancel: {
                    asked += 1
                    return true
                })
        ) { error in
            XCTAssertEqual(error as? CyberError, .cancelled)
        }
        XCTAssertGreaterThan(asked, 0, "the cancel callback never reached the engine")
    }

    func testAnIdMapsTableArrivesWithItsPixels() throws {
        let low = try unwrappedPlane()
        let high = try plane()
        let output = try low.bakeThroughProvider(
            from: high, map: .objectId, parameters: parameters())

        XCTAssertEqual(output.encoding.basis, .idColor)
        // No id column on a procedurally built Target, so the map falls back to
        // face-connected components and reports which source it used.
        XCTAssertFalse(output.encoding.idSource.isEmpty)
        XCTAssertFalse(output.encoding.idColors.isEmpty)
        for row in output.encoding.idColors {
            XCTAssertFalse(row.color == (0, 0, 0), "black is reserved for 'no id'")
        }

        // A texel picked out of the map resolves to exactly one row, at zero
        // tolerance — the property that makes an id map a selection mask.
        let centre = ((Int(output.height) / 2) * Int(output.width) + Int(output.width) / 2) * 3
        let picked = (
            UInt8(min(255, max(0, Int((output.pixels[centre] * 255).rounded())))),
            UInt8(min(255, max(0, Int((output.pixels[centre + 1] * 255).rounded())))),
            UInt8(min(255, max(0, Int((output.pixels[centre + 2] * 255).rounded()))))
        )
        XCTAssertEqual(output.encoding.idColors.filter { $0.color == picked }.count, 1)
    }

    func testAMapThisBuildCannotProduceIsRefusedByName() throws {
        let low = try unwrappedPlane()
        let high = try plane()
        XCTAssertThrowsError(
            try low.bakeThroughProvider(
                from: high, map: BakeMap(rawValue: 4242), parameters: parameters(size: 8))
        ) { error in
            // Named and listed, never substituted with a neutral image.
            XCTAssertTrue("\(error)".contains("4242"), "\(error)")
            XCTAssertTrue("\(error)".contains("material-id"), "\(error)")
        }
    }
}
