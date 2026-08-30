// Runtime parity checks for the ZRemesher surface.
//
// The `swift-package` CI job compiled this package but never ran it, and
// `swift_abi_parity` only proves the Swift sources reference symbols that
// exist. Neither catches the failures that actually bite a binding: a pointer
// that dies before the engine reads it, a `char[32]` decoded wrong, or a mode
// that is marshalled into a struct the engine never looks at. All three
// compile perfectly.
//
// So these assert the same properties `python_test_zremesher_bindings` asserts,
// on the same procedural fixture, which is what keeps the two surfaces from
// drifting apart. Where a number appears here it is the number the Python gate
// measures too.

import XCTest

@testable import CyberRemesher

// No "engine absent" skip here, deliberately. The Python suite exits 77 because
// it dlopens the library and can find it missing; this binary LINKS it, so a
// missing engine means the process never reaches main and the CI job's build
// step is what guarantees it exists. A skip guard would be one that can never
// fire.

private func makeSphere(rings: Int = 24, segments: Int = 36) throws -> Mesh {
    var lines: [String] = []
    for i in 0...rings {
        let theta = Double.pi * Double(i) / Double(rings)
        for j in 0..<segments {
            let phi = 2.0 * Double.pi * Double(j) / Double(segments)
            lines.append(
                String(
                    format: "v %.6f %.6f %.6f",
                    sin(theta) * cos(phi), cos(theta), sin(theta) * sin(phi)))
        }
    }
    func at(_ i: Int, _ j: Int) -> Int { i * segments + (j % segments) + 1 }
    for i in 0..<rings {
        for j in 0..<segments {
            lines.append("f \(at(i, j)) \(at(i + 1, j)) \(at(i + 1, j + 1)) \(at(i, j + 1))")
        }
    }
    let path = NSTemporaryDirectory() + "cyber_swift_parity_sphere.obj"
    try lines.joined(separator: "\n").write(toFile: path, atomically: true, encoding: .utf8)
    return try Mesh.loadOBJ(path)
}

/// A tilted ring. Nothing about a sphere wants a loop there, so a guide mode
/// that is genuinely honoured has to change the result.
private func tiltedRing(samples: Int = 48) -> [Float] {
    var points: [Float] = []
    for k in 0..<samples {
        let a = 2.0 * Float.pi * Float(k) / Float(samples)
        let x = cos(a)
        let z = sin(a)
        let y = 0.35 * cos(a)
        let n = (x * x + y * y + z * z).squareRoot()
        points += [x / n, y / n, z / n]
    }
    return points
}

private func zremesherParams(targetQuads: Int = 800) -> RemeshParameters {
    var params = RemeshParameters(targetQuads: targetQuads)
    params.quadMethod = .zremesher
    return params
}

final class ZRemesherParityTests: XCTestCase {
    /// Defaults come from the engine, not from restated literals, so the Swift
    /// mirror cannot drift from `cyber_default_zremesher_params`.
    func testDefaultsComeFromTheEngine() {
        let defaults = ZRemesherParameters()
        XCTAssertEqual(defaults.quality, .fast)
        XCTAssertEqual(defaults.symmetry, .none)
        XCTAssertTrue(defaults.boundaryChains)
        XCTAssertTrue(defaults.foldRepair)
        // Off deliberately: measured to make thin-feature survival WORSE.
        XCTAssertFalse(defaults.unifiedSizing)
    }

    /// The engine-bindings scenario: a quality mode and a symmetry axis, with
    /// the layout statistics and the score coming back as values.
    func testQualityAndSymmetryReachTheEngine() async throws {
        let mesh = try makeSphere()
        let result = try await mesh.remesh(
            params: zremesherParams(),
            zremesher: ZRemesherParameters(quality: .best, symmetry: .x)
        ).value()
        let report = result.report

        XCTAssertGreaterThan(report.layouts, 0, "no topology layout was traced")
        XCTAssertEqual(report.layouts, report.layoutsValid, "a layout failed its own invariants")
        XCTAssertGreaterThan(report.nodes, 0)
        XCTAssertGreaterThan(report.arcs, 0)
        XCTAssertGreaterThan(report.patches, 0)

        // Decoded out of the C `char[32]`, which is where a tuple-to-String
        // mistake would show as trailing NULs or an empty name.
        XCTAssertTrue(
            ["multires", "single-level"].contains(report.selectedCandidate),
            "quality .best selected \(report.selectedCandidate.debugDescription)")
        XCTAssertGreaterThan(report.qualityScore, 0)

        XCTAssertTrue(report.symmetryApplied)
        XCTAssertTrue(
            report.topologicallySymmetric,
            "symmetry .x produced a mesh that is not topologically symmetric")
        XCTAssertGreaterThan(report.mirroredFaces, 0)
    }

    /// `.fast` selects nothing, and an empty name must mean exactly that rather
    /// than a decode that silently produced an empty string.
    func testFastSelectsNoCandidate() async throws {
        let mesh = try makeSphere()
        let result = try await mesh.remesh(
            params: zremesherParams(), zremesher: ZRemesherParameters()
        ).value()
        XCTAssertTrue(result.report.selectedCandidate.isEmpty)
        XCTAssertGreaterThan(result.report.layouts, 0, "the run still traces a layout")
    }

    /// Asserted through an OUTPUT difference, not by trusting the struct: if
    /// the two modes produce the same mesh, the mode never reached the engine.
    func testGuideModeChangesTheResult() async throws {
        let mesh = try makeSphere()
        let ring = tiltedRing()

        let orientation = try await mesh.remesh(
            params: zremesherParams(), zremesher: ZRemesherParameters(),
            guidance: Guidance(guides: [
                FlowGuide(points: ring, radius: 0.25, mode: .orientation, closed: true)
            ])
        ).value()
        let topology = try await mesh.remesh(
            params: zremesherParams(), zremesher: ZRemesherParameters(),
            guidance: Guidance(guides: [
                FlowGuide(points: ring, radius: 0.25, mode: .topology, closed: true)
            ])
        ).value()

        XCTAssertNotEqual(
            orientation.mesh.positions().count, topology.mesh.positions().count,
            "orientation and topology guides produced identical output — the mode was dropped")
    }

    /// Two guides exercise the recursion in `withGuidance` past its first
    /// level, which is where a stroke's buffer would be freed before the engine
    /// read it if the scopes were not nested.
    func testSeveralGuidesKeepTheirBuffersAlive() async throws {
        let mesh = try makeSphere()
        let result = try await mesh.remesh(
            params: zremesherParams(), zremesher: ZRemesherParameters(),
            guidance: Guidance(guides: [
                FlowGuide(points: tiltedRing(), radius: 0.25, mode: .topology, closed: true),
                FlowGuide(points: [0, 1, 0, 1, 0, 0, 0, -1, 0], radius: 0.2),
            ])
        ).value()
        XCTAssertGreaterThan(result.mesh.positions().count, 0)
        XCTAssertGreaterThan(result.report.layouts, 0)
    }

    /// Rejected, never reinterpreted. A symmetry axis quietly clamped to "none"
    /// hands back an asymmetric mesh for a symmetry request.
    func testUnknownModesAreRejected() async throws {
        let mesh = try makeSphere(rings: 8, segments: 12)
        let params = zremesherParams(targetQuads: 200)

        await assertInvalidParameter("quality") {
            _ = try await mesh.remesh(
                params: params,
                zremesher: ZRemesherParameters(quality: ZRemesherQuality(rawValue: 7))
            ).value()
        }
        await assertInvalidParameter("symmetry axis") {
            _ = try await mesh.remesh(
                params: params,
                zremesher: ZRemesherParameters(symmetry: SymmetryAxis(rawValue: 9))
            ).value()
        }
        await assertInvalidParameter("guide mode") {
            _ = try await mesh.remesh(
                params: params, zremesher: ZRemesherParameters(),
                guidance: Guidance(guides: [
                    FlowGuide(
                        points: [0, 0, 0, 1, 0, 0], radius: 0.3,
                        mode: GuideMode(rawValue: 42))
                ])
            ).value()
        }
    }

    /// The ABI reads `3 * point_count` floats through a pointer with an implied
    /// length, so a ragged stroke would be read past the end of its own buffer.
    /// Caught in Swift, before the call.
    func testRaggedPointBufferIsRejectedBeforeTheCall() async throws {
        let mesh = try makeSphere(rings: 8, segments: 12)
        do {
            _ = try await mesh.remesh(
                params: zremesherParams(targetQuads: 200), zremesher: ZRemesherParameters(),
                guidance: Guidance(guides: [FlowGuide(points: [0, 0, 0, 1, 0], radius: 0.3)])
            ).value()
            XCTFail("a ragged point buffer was accepted")
        } catch let error as CyberError {
            guard case .invalidArgument = error else {
                return XCTFail("ragged points gave the wrong error: \(error)")
            }
        }
    }

    private func assertInvalidParameter(
        _ what: String,
        file: StaticString = #filePath,
        line: UInt = #line,
        _ body: () async throws -> Void
    ) async {
        do {
            try await body()
            XCTFail("an unknown \(what) was accepted instead of rejected", file: file, line: line)
        } catch let error as CyberError {
            guard case .invalidParameter = error else {
                return XCTFail(
                    "unknown \(what) gave the wrong error: \(error)", file: file, line: line)
            }
        } catch {
            XCTFail("unknown \(what) threw \(error)", file: file, line: line)
        }
    }
}
