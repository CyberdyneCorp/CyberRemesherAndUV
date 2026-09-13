import CyberRemesher

@main
enum CyberRemesherConsumer {
    static func main() async {
        do {
            try CyberRuntime.checkABI()
            let mesh = try Mesh(
                positions: [0, 0, 0, 1, 0, 0, 1, 1, 0, 0, 1, 0],
                faceOffsets: [0, 3, 6],
                indices: [0, 1, 2, 0, 2, 3]
            )
            let completed = mesh.remesh(params: RemeshParameters(targetQuads: 4))
            let progress = Task { () -> Bool in
                for await _ in completed.progress { return true }
                return false
            }
            let result = try await completed.value()
            let polygons = result.authoredPolygons()
            guard result.faceCount > 0, polygons.faceOffsets.count > 1,
                  polygons.indices.count >= 3 else {
                fatalError("remesh returned no authored polygons")
            }
            _ = await progress.value

            let cancelled = mesh.remesh(params: RemeshParameters(targetQuads: 4))
            cancelled.cancel()
            do {
                _ = try await cancelled.value()
                fatalError("a pre-cancelled operation unexpectedly succeeded")
            } catch CyberError.cancelled {
                print("CYBER_IOS_CONSUMER_OK")
            }
        } catch {
            fatalError("CyberRemesher consumer failed: \(error)")
        }
    }
}
