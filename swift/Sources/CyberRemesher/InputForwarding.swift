// UIKit + PencilKit event capture. Converts native touch/pencil input into
// engine `StrokeSample`s in normalized viewport coordinates and assembles them
// into a `Stroke` the caller hands to `StrokeInterpretation`.
//
// The C ABI has no session to inject into — interpretation is a pure function
// of one completed stroke plus the mesh and view matrix (see
// StrokeInterpretation.swift) — so this type is a pure SAMPLER: it owns no
// engine state and never calls the ABI. The caller supplies the view matrix,
// because that changes per frame and only the renderer knows it.
//
// The whole file compiles out on platforms without UIKit so the package still
// resolves on macOS-AppKit and Linux tooling.

#if canImport(UIKit)
import UIKit

/// Collects UIKit touch/pencil events for a bound view into engine strokes.
///
/// Attach it to your gesture pipeline and call the capture methods from your
/// `UIView` / `UIGestureRecognizer` callbacks. Coordinates are normalized
/// against `view.bounds` so the engine stays resolution-independent.
///
/// ```swift
/// forwarder.begin()
/// forwarder.append(touches, coalescedFrom: event)
/// let interpretation = try StrokeInterpretation(
///     stroke: forwarder.end(), mesh: mesh, viewProjection: renderer.viewProjection)
/// ```
public final class TouchInputForwarder {
    // Same-file extensions (incl. the PencilKit one below) can reach these:
    // Swift `private` is file-scoped for extensions of the same type.
    private weak var view: UIView?
    private var buffer: [StrokeSample] = []
    private var startTimestamp: TimeInterval?

    public init(view: UIView) {
        self.view = view
    }

    /// Viewport width / height of the bound view (1 when it has no extent).
    public var aspect: Float {
        guard let view, view.bounds.height > 0 else { return 1 }
        return Float(view.bounds.width / view.bounds.height)
    }

    /// Begins a new stroke, discarding any partial buffer.
    public func begin() {
        buffer.removeAll(keepingCapacity: true)
        startTimestamp = nil
    }

    /// Appends touches from a began/moved phase to the in-flight stroke,
    /// expanding coalesced (high-frequency) touches when the event provides them.
    public func append(_ touches: Set<UITouch>, coalescedFrom event: UIEvent?) {
        guard let view else { return }
        for touch in touches {
            let coalesced = event?.coalescedTouches(for: touch) ?? [touch]
            for sample in coalesced {
                if startTimestamp == nil { startTimestamp = sample.timestamp }
                buffer.append(
                    TouchInputForwarder.sample(
                        from: sample, in: view, since: startTimestamp ?? sample.timestamp))
            }
        }
    }

    /// Ends the stroke and returns it, resetting the buffer.
    public func end() -> Stroke {
        defer {
            buffer.removeAll(keepingCapacity: true)
            startTimestamp = nil
        }
        return Stroke(samples: buffer, aspect: aspect)
    }

    /// A discrete tap (e.g. from a `UITapGestureRecognizer`) as a one-sample
    /// stroke — the grammar reads it as a stationary hold point.
    public func tap(at location: CGPoint) -> Stroke {
        guard let view else { return Stroke(samples: [], aspect: 1) }
        let sample = StrokeSample(location: TouchInputForwarder.point(location, in: view))
        return Stroke(samples: [sample], aspect: aspect)
    }

    // MARK: - Mapping

    private static func sample(
        from touch: UITouch, in view: UIView, since start: TimeInterval
    ) -> StrokeSample {
        let location = point(touch.location(in: view), in: view)
        let normalizedPressure = touch.maximumPossibleForce > 0
            ? Double(touch.force / touch.maximumPossibleForce)
            : 1.0
        return StrokeSample(
            location: location,
            pressure: normalizedPressure,
            altitudeAngle: Double(touch.altitudeAngle),
            azimuthAngle: Double(touch.azimuthAngle(in: view)),
            timestamp: touch.timestamp - start
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
    /// Converts a finished PencilKit stroke (from a `PKCanvasView` drawing)
    /// into one engine stroke by sampling its interpolated path.
    ///
    /// - Parameters:
    ///   - stroke: the PencilKit stroke to replay.
    ///   - view: the view whose bounds define the normalization frame.
    func stroke(from stroke: PKStroke, in view: UIView) -> Stroke {
        let path = stroke.path
        var samples: [StrokeSample] = []
        samples.reserveCapacity(path.count)
        for point in path {
            samples.append(
                StrokeSample(
                    location: TouchInputForwarder.point(point.location, in: view),
                    pressure: Double(point.force),
                    altitudeAngle: Double(point.altitude),
                    azimuthAngle: Double(point.azimuth),
                    timestamp: point.timeOffset
                )
            )
        }
        return Stroke(samples: samples, aspect: aspect)
    }
}
#endif // canImport(UIKit) && canImport(PencilKit)
