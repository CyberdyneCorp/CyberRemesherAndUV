// Gesture stroke interpretation over the real `cyber_stroke_interpret` family.
//
// The C ABI has NO session object: interpretation is a pure function of one
// completed stroke plus the mesh and view matrix to resolve it against, and it
// returns an opaque, immutable record of ranked candidates. This file wraps
// that record; the caller stays the owner of the document, undo stack and
// renderer, and applies the chosen candidate with the `cyber_retopo_*` ops
// (`Mesh` + `SoftSelection.swift`).
//
// Coordinates are NORMALIZED viewport coordinates (0..1, origin top-left) —
// resolution-independent, which is what lets a recorded synthetic trace and a
// real UITouch produce identical interpretations.

import CCyberRemesher

/// A normalized 2D point in viewport space (`0...1`, origin top-left).
public struct StrokePoint: Sendable, Equatable {
    public var x: Double
    public var y: Double
    public init(x: Double, y: Double) {
        self.x = x
        self.y = y
    }
}

/// One sample along an input stroke.
///
/// Carries Pencil-grade data (pressure and tilt) so pressure-driven tools and
/// palm rejection behave identically for a real `UITouch` and a recorded
/// synthetic trace. The interpreter itself consumes only `location` and
/// `timestamp` — the ABI takes x, y, t triplets — while `pressure` feeds the
/// brush ops (`Mesh.paintSelection`, erase) once a candidate is applied.
public struct StrokeSample: Sendable {
    public var location: StrokePoint
    /// Normalized pressure `0...1` (1.0 for non-pressure devices).
    public var pressure: Double
    /// Pencil altitude angle in radians (`.pi/2` for a perpendicular stylus).
    public var altitudeAngle: Double
    /// Pencil azimuth angle in radians.
    public var azimuthAngle: Double
    /// Seconds since the stroke began.
    public var timestamp: Double

    public init(
        location: StrokePoint,
        pressure: Double = 1.0,
        altitudeAngle: Double = .pi / 2,
        azimuthAngle: Double = 0.0,
        timestamp: Double = 0.0
    ) {
        self.location = location
        self.pressure = pressure
        self.altitudeAngle = altitudeAngle
        self.azimuthAngle = azimuthAngle
        self.timestamp = timestamp
    }
}

/// A completed stroke ready for interpretation.
public struct Stroke: Sendable {
    public var samples: [StrokeSample]
    /// Viewport width / height, so circles and angles are measured undistorted.
    public var aspect: Float

    public init(samples: [StrokeSample], aspect: Float = 1) {
        self.samples = samples
        self.aspect = aspect
    }
}

/// Stage-1 shape class of a stroke.
public enum StrokeShape: Equatable, Sendable {
    case unknown
    /// Stationary press (tap/hold).
    case holdPoint
    case line
    /// Closed polygon with corners (quad draw).
    case closedLoop
    case circle
    /// Open, many reversals or self-crossings.
    case scribble
    /// An X drawn in one stroke.
    case cross
    /// Closed, irregular.
    case lasso
    /// Open square wave (one-stroke grid).
    case grid

    init(_ raw: CyberStrokeShape) {
        switch raw {
        case CYBER_SHAPE_HOLD_POINT: self = .holdPoint
        case CYBER_SHAPE_LINE: self = .line
        case CYBER_SHAPE_CLOSED_LOOP: self = .closedLoop
        case CYBER_SHAPE_CIRCLE: self = .circle
        case CYBER_SHAPE_SCRIBBLE: self = .scribble
        case CYBER_SHAPE_CROSS: self = .cross
        case CYBER_SHAPE_LASSO: self = .lasso
        case CYBER_SHAPE_GRID: self = .grid
        default: self = .unknown
        }
    }
}

/// Stage-2 under-stroke context: what the stroke was drawn over.
public enum StrokeContext: Equatable, Sendable {
    case emptySurface
    case face
    case edge
    case boundaryEdge
    case vertex

    init(_ raw: CyberStrokeContext) {
        switch raw {
        case CYBER_CONTEXT_FACE: self = .face
        case CYBER_CONTEXT_EDGE: self = .edge
        case CYBER_CONTEXT_BOUNDARY_EDGE: self = .boundaryEdge
        case CYBER_CONTEXT_VERTEX: self = .vertex
        default: self = .emptySurface
        }
    }
}

/// An editing verb the grammar proposes for a stroke.
public enum StrokeAction: Equatable, Sendable {
    case none
    case createQuad
    case insertLoop
    case tagLoop
    case dissolveEdge
    case deleteFaces
    case mergeVertices
    case rotateEdge
    case tweakVertex
    case hideRegion
    case toggleVisibility
    case createGrid

    init(_ raw: CyberStrokeAction) {
        switch raw {
        case CYBER_ACTION_CREATE_QUAD: self = .createQuad
        case CYBER_ACTION_INSERT_LOOP: self = .insertLoop
        case CYBER_ACTION_TAG_LOOP: self = .tagLoop
        case CYBER_ACTION_DISSOLVE_EDGE: self = .dissolveEdge
        case CYBER_ACTION_DELETE_FACES: self = .deleteFaces
        case CYBER_ACTION_MERGE_VERTICES: self = .mergeVertices
        case CYBER_ACTION_ROTATE_EDGE: self = .rotateEdge
        case CYBER_ACTION_TWEAK_VERTEX: self = .tweakVertex
        case CYBER_ACTION_HIDE_REGION: self = .hideRegion
        case CYBER_ACTION_TOGGLE_VISIBILITY: self = .toggleVisibility
        case CYBER_ACTION_CREATE_GRID: self = .createGrid
        default: self = .none
        }
    }
}

/// Kind of a referenced mesh element.
public enum ElementKind: Equatable, Sendable {
    case vertex
    case edge
    case face

    init(_ raw: CyberElementKind) {
        switch raw {
        case CYBER_ELEMENT_EDGE: self = .edge
        case CYBER_ELEMENT_FACE: self = .face
        default: self = .vertex
        }
    }
}

/// A mesh element a candidate would touch. Ids are stable engine element ids,
/// never compacted render-buffer indices.
public struct ElementRef: Equatable, Sendable {
    public var kind: ElementKind
    public var id: UInt32
}

/// The interpretation of one completed stroke: a shape class, the context it
/// was drawn over, and the ranked candidate actions (index 0 is the chosen one,
/// the rest are the one-tap alternatives).
///
/// Deterministic: identical inputs produce an identical record.
public final class StrokeInterpretation {
    let handle: OpaquePointer

    /// Interprets `stroke` against `mesh`.
    ///
    /// - Parameters:
    ///   - stroke: the completed stroke; needs at least one sample.
    ///   - mesh: the EditMesh to resolve context against, or `nil` (stage 1
    ///     still runs; every context rule then sees an empty scene).
    ///   - viewProjection: column-major 4x4 world->clip matrix (16 floats, the
    ///     `simd_float4x4` memory order). Required whenever `mesh` is non-nil.
    public init(stroke: Stroke, mesh: Mesh?, viewProjection: [Float]?) throws {
        guard !stroke.samples.isEmpty else {
            throw CyberError.invalidArgument("a stroke needs at least one sample")
        }
        if mesh != nil, viewProjection?.count != 16 {
            throw CyberError.invalidArgument(
                "interpreting against a mesh needs a 16-float view-projection matrix")
        }
        var triplets = [Float]()
        triplets.reserveCapacity(stroke.samples.count * 3)
        for sample in stroke.samples {
            triplets.append(Float(sample.location.x))
            triplets.append(Float(sample.location.y))
            triplets.append(Float(sample.timestamp))
        }

        var out: OpaquePointer?
        let status = StrokeInterpretation.withViewProjection(viewProjection) { viewProj in
            triplets.withUnsafeBufferPointer { samples in
                cyber_stroke_interpret(
                    mesh?.handle, viewProj,
                    samples.baseAddress, stroke.samples.count, stroke.aspect,
                    &out)
            }
        }
        try CyberError.check(status)
        guard let handle = out else {
            throw CyberError.outOfMemory
        }
        self.handle = handle
        withExtendedLifetime(mesh) {}
    }

    deinit {
        cyber_stroke_interpretation_free(handle)
    }

    /// Stage-1 shape class and its heuristic confidence (`0...1`).
    public var shape: StrokeShape {
        StrokeShape(cyber_stroke_interpretation_shape(handle))
    }

    public var shapeConfidence: Float {
        cyber_stroke_interpretation_shape_confidence(handle)
    }

    /// Stage-2 under-stroke context.
    public var context: StrokeContext {
        StrokeContext(cyber_stroke_interpretation_context(handle))
    }

    /// Ranked candidates, best first.
    public var candidateCount: Int {
        Int(cyber_stroke_interpretation_candidate_count(handle))
    }

    public func action(_ candidate: Int) -> StrokeAction {
        StrokeAction(cyber_stroke_interpretation_action(handle, candidate))
    }

    public func confidence(_ candidate: Int) -> Float {
        cyber_stroke_interpretation_confidence(handle, candidate)
    }

    /// Mesh elements `candidate` would touch, in deterministic order.
    public func elements(of candidate: Int) -> [ElementRef] {
        let count = Int(cyber_stroke_interpretation_element_count(handle, candidate))
        return (0..<count).compactMap { index in
            var kind = CYBER_ELEMENT_VERTEX
            var id: UInt32 = 0
            guard cyber_stroke_interpretation_element(handle, candidate, index, &kind, &id) == 1
            else { return nil }
            return ElementRef(kind: ElementKind(kind), id: id)
        }
    }

    /// Estimated corner points of a CLOSED stroke, ordered as a simple ring —
    /// the quad the shell should build for a `.createQuad` candidate (the shell
    /// unprojects them onto the Target). Empty for open shapes.
    ///
    /// For a `.grid` stroke this instead holds the lattice points, row-major;
    /// see ``gridSize``.
    public func corners() -> [StrokePoint] {
        let count = Int(cyber_stroke_interpretation_corner_count(handle))
        return (0..<count).compactMap { index in
            var xy = [Float](repeating: 0, count: 2)
            guard cyber_stroke_interpretation_corner(handle, index, &xy) == 1 else { return nil }
            return StrokePoint(x: Double(xy[0]), y: Double(xy[1]))
        }
    }

    /// Quad-cell counts of a GRID stroke's estimated lattice, or `nil` for any
    /// other shape. ``corners()`` then holds `(rows + 1) * (cols + 1)` points.
    public var gridSize: (rows: Int, cols: Int)? {
        var rows = 0
        var cols = 0
        guard cyber_stroke_interpretation_grid_size(handle, &rows, &cols) == 1 else { return nil }
        return (rows, cols)
    }

    // MARK: - Private

    /// Runs `body` with the view-projection matrix as a C pointer, or NULL when
    /// there is none (which the ABI allows only for a NULL mesh).
    private static func withViewProjection<T>(
        _ matrix: [Float]?, _ body: (UnsafePointer<Float>?) -> T
    ) -> T {
        guard let matrix else { return body(nil) }
        return matrix.withUnsafeBufferPointer { body($0.baseAddress) }
    }
}
