// UNVERIFIED: requires the Swift toolchain, the `cyber_capi` library, and
// QuartzCore/Metal (Apple platforms only); not buildable in headless Linux CI.
// Written against the ABI contract in README.md.
//
// CAMetalLayer viewport attachment. The engine renders into a caller-owned
// `CAMetalLayer`; this type hands the layer's pointer across the C ABI and
// forwards drawable-size changes. This is a *placeholder* seam: it wires the
// ownership/lifecycle correctly, but the actual Metal device/queue selection
// and frame scheduling live in the engine's render backend (viewport-rendering
// spec) and are exercised only on real Metal hardware.

#if canImport(QuartzCore) && canImport(Metal)
import CCyberRemesher
import QuartzCore
import Metal

/// Attaches an engine ``Session`` to a `CAMetalLayer` for on-device rendering.
///
/// Keep a strong reference for the lifetime of the viewport; ``detach()`` (or
/// deinit) tears the engine's swapchain down.
public final class MetalViewport {
    private let session: Session
    private let layer: CAMetalLayer
    private var attached = false

    /// - Parameters:
    ///   - session: the editing session to render.
    ///   - layer: the target layer. Its `device` is set to the engine's Metal
    ///     device if not already configured.
    public init(session: Session, layer: CAMetalLayer) {
        self.session = session
        self.layer = layer
    }

    deinit {
        try? detach()
    }

    /// Binds the layer to the engine render backend.
    ///
    /// - Throws: ``CyberError`` if the engine rejects the layer (e.g. no Metal
    ///   device available on this platform build).
    public func attach() throws {
        guard !attached else { return }
        if layer.device == nil {
            layer.device = MTLCreateSystemDefaultDevice()
        }
        layer.pixelFormat = .bgra8Unorm
        layer.framebufferOnly = true

        let layerPointer = Unmanaged.passUnretained(layer).toOpaque()
        try CyberError.check(cyber_session_attach_metal_layer(session.handle, layerPointer))
        attached = true
    }

    /// Informs the engine of a new drawable size (call from `layoutSubviews`).
    public func setDrawableSize(width: Double, height: Double) throws {
        layer.drawableSize = CGSize(width: width, height: height)
        guard attached else { return }
        try CyberError.check(
            cyber_session_set_drawable_size(session.handle, width, height)
        )
    }

    /// Detaches the layer and releases the engine swapchain.
    public func detach() throws {
        guard attached else { return }
        attached = false
        try CyberError.check(cyber_session_detach_metal_layer(session.handle))
    }
}
#endif // canImport(QuartzCore) && canImport(Metal)
