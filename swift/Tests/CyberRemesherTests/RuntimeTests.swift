import CCyberRemesher
import XCTest

@testable import CyberRemesher

/// The ABI version the package reports, and the check built on it.
///
/// `abiVersionCompiledAgainst` used to be a hand-kept `(1, 7)` that never moved
/// while the header went to 1.24. Nothing caught it: a 1.7 client is always
/// served by a 1.x library, so `checkABI()` kept passing. Only the 2.0 bump
/// exposed it, as a refused iOS consumer in CI. Python has had the equivalent
/// gate (`python_test_abi_contract`) all along; this is the Swift half.
final class RuntimeTests: XCTestCase {
    func testCompiledAgainstIsTheHeaderTheLibraryWasBuiltFrom() {
        // The package and the library in this tree are built from the same
        // cyber_capi.h, so a correct compiled-against version equals the
        // version the linked library reports. A stale constant fails here on
        // the first minor bump, instead of years later on the next major.
        let compiled = CyberRuntime.abiVersionCompiledAgainst
        let linked = CyberRuntime.abiVersionComponents
        XCTAssertEqual(compiled.major, linked.major)
        XCTAssertEqual(compiled.minor, linked.minor)
    }

    func testTheLinkedLibraryServesThisPackage() {
        XCTAssertNoThrow(try CyberRuntime.checkABI())
    }

    func testAClientOfThePreviousMajorIsRefused() {
        // The compatibility rule lives in the engine; this proves the binding
        // surfaces a refusal as the typed error rather than swallowing it.
        let previousMajor = Int32(CyberRuntime.abiVersionComponents.major - 1)
        XCTAssertThrowsError(try CyberError.check(cyber_abi_check(previousMajor, 99))) { error in
            guard let typed = error as? CyberError, case .incompatibleVersion = typed else {
                return XCTFail("expected incompatibleVersion, got \(error)")
            }
        }
    }
}
