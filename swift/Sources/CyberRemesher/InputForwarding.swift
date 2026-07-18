// UNVERIFIED: requires the Swift toolchain, the `cyber_capi` library, and
// UIKit/PencilKit (Apple platforms only); not buildable in headless Linux CI.
// Written against the ABI contract in README.md.
//
// UIKit + PencilKit event forwarding. Converts native touch/pencil input into
// engine `StrokeSample`s in normalized viewport coordinates and injects them
// into a `Session`. The whole file compiles out on platforms without UIKit so
// the package still resolves on macOS-AppKit / Linux tooling.

#if canImport(UIKit)
import UIKit

/// Bridges UIKit touch/pencil events for a bound viewport into a ``Session``.
///
/// Attach it to your gesture pipeline and call the `forward` methods from your
/// `UIView`/`UIGestureRecognizer` callbacks. Coordinates are normalized against
/// `view.bounds` so the engine stays resolution-independent.
public final class TouchInputForwarder {
    // Same-file extensions (incl. the PencilKit one below) can reach these:
    // Swift `private` is file-scoped for extensions of the same type.
    private let session: Session
    private weak var view: UIView?
    private var buffer: [StrokeSample] = []

    public init(session: Session, view: UIView) {
        self.session = session
        self.view = view
    }

    /// Begins a new stroke, discarding any partial buffer.
    public func begin() {
        buffer.removeAll(keepingCapacity: true)
    }

    /// Appends touches from a began/moved phase to the in-flight stroke,
    /// expanding coalesced (high-frequency) touches when the event provides them.
    public func append(_ touches: Set<UITouch>, coalescedFrom event: UIEvent?) {
        guard let view else { return }
        for touch in touches {
            let coalesced = event?.coalescedTouches(for: touch) ?? [touch]
            for sample in coalesced {
                buffer.append(TouchInputForwarder.sample(from: sample, in: view))
            }
        }
    }

    /// Ends the stroke and injects it; an empty stroke is dropped.
    public func end() throws {
        defer { buffer.removeAll(keepingCapacity: true) }
        try session.inject(stroke: buffer)
    }

    /// Forwards a discrete tap (e.g. from a `UITapGestureRecognizer`).
    public func forwardTap(at location: CGPoint, in view: UIView) throws {
        try session.inject(tapAt: TouchInputForwarder.point(location, in: view))
    }

    // MARK: - Mapping

    private static func sample(from touch: UITouch, in view: UIView) -> StrokeSample {
        let location = point(touch.location(in: view), in: view)
        let normalizedPressure = touch.maximumPossibleForce > 0
            ? Double(touch.force / touch.maximumPossibleForce)
            : 1.0
        return StrokeSample(
            location: location,
            pressure: normalizedPressure,
            altitudeAngle: Double(touch.altitudeAngle),
            azimuthAngle: Double(touch.azimuthAngle(in: view)),
            timestamp: touch.timestamp
        )
    }

    static func point(_ location: CGPoint, in view: UIView) -> StrokePoint {
        let width = view.bounds.width
        let height = view.bounds.height
        let x = width > 0 ? Double(location.x / width) : 0
        let y = height > 0 ? Double(location.y / height) : 0
        return StrokePoint(x: x, y: y)
    }
}
#endif // canImport(UIKit)

#if canImport(UIKit) && canImport(PencilKit)
import PencilKit

public extension TouchInputForwarder {
    /// Forwards a finished PencilKit stroke (from a `PKCanvasView` drawing) by
    /// sampling its interpolated path and injecting it as one engine stroke.
    ///
    /// - Parameters:
    ///   - stroke: the PencilKit stroke to replay.
    ///   - view: the view whose bounds define the normalization frame.
    func forward(pencilStroke stroke: PKStroke, in view: UIView) throws {
        let path = stroke.path
        guard !path.isEmpty else { return }
        var samples: [StrokeSample] = []
        samples.reserveCapacity(path.count)
        for point in path {
            let mapped = TouchInputForwarder.point(point.location, in: view)
            samples.append(
                StrokeSample(
                    location: mapped,
                    pressure: Double(point.force),
                    altitudeAngle: Double(point.altitude),
                    azimuthAngle: Double(point.azimuth),
                    timestamp: point.timeOffset
                )
            )
        }
        try session.inject(stroke: samples)
    }
}
#endif // canImport(UIKit) && canImport(PencilKit)
