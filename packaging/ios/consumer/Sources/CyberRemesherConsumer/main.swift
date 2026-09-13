import CyberRemesher
import Darwin
import Foundation

@main
enum CyberRemesherConsumer {
    static func residentBytes() -> UInt64 {
        var info = mach_task_basic_info()
        var count = mach_msg_type_number_t(MemoryLayout<mach_task_basic_info>.size /
                                           MemoryLayout<natural_t>.size)
        let status = withUnsafeMutablePointer(to: &info) {
            $0.withMemoryRebound(to: integer_t.self, capacity: Int(count)) {
                task_info(mach_task_self_, task_flavor_t(MACH_TASK_BASIC_INFO), $0, &count)
            }
        }
        return status == KERN_SUCCESS ? UInt64(info.resident_size) : 0
    }

    static func thermalState() -> String {
        switch ProcessInfo.processInfo.thermalState {
        case .nominal: "nominal"
        case .fair: "fair"
        case .serious: "serious"
        case .critical: "critical"
        @unknown default: "unknown"
        }
    }

    static func main() async {
        do {
            try CyberRuntime.checkABI()
            let mesh = try Mesh(
                positions: [0, 0, 0, 1, 0, 0, 1, 1, 0, 0, 1, 0],
                faceOffsets: [0, 3, 6],
                indices: [0, 1, 2, 0, 2, 3]
            )
            let rssBefore = residentBytes()
            let thermalBefore = thermalState()
            let sampler = Task { () -> UInt64 in
                var peak = rssBefore
                while !Task.isCancelled {
                    peak = max(peak, residentBytes())
                    try? await Task.sleep(nanoseconds: 5_000_000)
                }
                return max(peak, residentBytes())
            }
            let started = DispatchTime.now().uptimeNanoseconds
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
            let remeshElapsedMs = Double(DispatchTime.now().uptimeNanoseconds - started) / 1_000_000
            sampler.cancel()
            let rssPeak = await sampler.value
            let rssAfter = residentBytes()

            let cancelled = mesh.remesh(params: RemeshParameters(targetQuads: 4))
            let cancelStarted = DispatchTime.now().uptimeNanoseconds
            cancelled.cancel()
            do {
                _ = try await cancelled.value()
                fatalError("a pre-cancelled operation unexpectedly succeeded")
            } catch CyberError.cancelled {
                let cancelElapsedMs = Double(DispatchTime.now().uptimeNanoseconds - cancelStarted) / 1_000_000
                print("CYBER_IOS_METRICS remesh_elapsed_ms=\(remeshElapsedMs) rss_before_bytes=\(rssBefore) rss_peak_bytes=\(rssPeak) rss_after_bytes=\(rssAfter) cancel_mode=prestart cancel_elapsed_ms=\(cancelElapsedMs) thermal_before=\(thermalBefore) thermal_after=\(thermalState())")
                print("CYBER_IOS_CONSUMER_OK")
            }
        } catch {
            fatalError("CyberRemesher consumer failed: \(error)")
        }
    }
}
