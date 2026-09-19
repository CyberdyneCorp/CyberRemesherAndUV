import CCyberRemesher
import Foundation
import XCTest

@testable import CyberRemesher

/// UDIM tile detection and the per-tile bake, driven from Swift.
///
/// Compiling the package proves it references symbols that exist; it never
/// proves the marshalling works. A binding that hands the engine the wrong
/// pointer, loses the count on the second call of a two-call query, or reads a
/// tile number from the wrong index still returns CYBER_OK. So every assertion
/// here checks a property of the RESULT that only a correct marshalling
/// produces: the tile NUMBERS of a layout authored to occupy two known tiles,
/// one image per tile in the layout's order, and a refusal whose message names
/// which of the two texel ceilings it hit.
final class UdimBakeTests: XCTestCase {

    /// Two unit quads far apart in model space, with UVs authored into tiles
    /// 1001 and 1002. Written as an OBJ because that is the one route a Swift
    /// caller has to a mesh with a UV layout it chose itself.
    private func twoTileQuads(name: String) throws -> Mesh {
        let obj = """
            v 0 0 0
            v 1 0 0
            v 1 1 0
            v 0 1 0
            v 3 0 0
            v 4 0 0
            v 4 1 0
            v 3 1 0
            vt 0.1 0.1
            vt 0.9 0.1
            vt 0.9 0.9
            vt 0.1 0.9
            vt 1.1 0.1
            vt 1.9 0.1
            vt 1.9 0.9
            vt 1.1 0.9
            f 1/1 2/2 3/3 4/4
            f 5/5 6/6 7/7 8/8
            """
        let path = NSTemporaryDirectory() + "cyber_swift_udim_\(name).obj"
        try obj.write(toFile: path, atomically: true, encoding: .utf8)
        return try Mesh.loadOBJ(path)
    }

    private func parameters(size: Int32 = 16) -> BakeParameters {
        var params = BakeParameters()
        params.width = size
        params.height = size
        return params
    }

    func testTheOccupiedTilesArriveAscendingWithoutABake() throws {
        let mesh = try twoTileQuads(name: "tiles")
        let layout = try mesh.udimTiles()
        XCTAssertEqual(layout.tiles, [1001, 1002])
        XCTAssertEqual(layout.unaddressableFaces, 0)

        // A mesh with no UV layout reports nothing rather than throwing, which
        // is the contract a host calls this with before it has unwrapped.
        let bare = try Mesh(positions: [0, 0, 0, 1, 0, 0, 0, 1, 0], indices: [0, 1, 2])
        XCTAssertEqual(try bare.udimTiles(), UdimLayout(tiles: [], unaddressableFaces: 0))
    }

    func testEachOccupiedTileComesBackAsItsOwnImage() throws {
        let low = try twoTileQuads(name: "low")
        let high = try twoTileQuads(name: "high")

        let tiles = try low.bakeUdim(from: high, map: .normal, parameters: parameters())
        XCTAssertEqual(tiles.map(\.tile), [1001, 1002])
        for tile in tiles {
            XCTAssertEqual(tile.image.width, 16)
            XCTAssertEqual(tile.image.height, 16)
        }
    }

    func testBothTexelCeilingsRefuseByName() throws {
        let low = try twoTileQuads(name: "ceiling_low")
        let high = try twoTileQuads(name: "ceiling_high")
        let previous = cyber_max_bake_pixels()
        defer { _ = cyber_set_max_bake_pixels(previous) }

        // One 16x16 tile is 256 texels: 255 refuses the TILE, 300 fits one tile
        // and not two. Two problems, two messages.
        for (ceiling, word) in [(UInt64(255), "PER-TILE"), (UInt64(300), "AGGREGATE")] {
            _ = cyber_set_max_bake_pixels(ceiling)
            XCTAssertThrowsError(
                try low.bakeUdim(from: high, map: .normal, parameters: parameters())
            ) { error in
                XCTAssertTrue(
                    "\(error)".contains(word), "ceiling \(ceiling) said: \(error)")
            }
        }
    }
}
