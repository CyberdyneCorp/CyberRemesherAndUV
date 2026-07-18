// UNVERIFIED: Android shell — JNI bindings to the C ABI (task 8.5). Not built in
// CI; requires libcyber_capi + the NDK. Native handles are opaque Longs
// (reinterpreted pointers in cyber_jni.cpp). No engine logic here: this is a
// 1:1 thin surface over the assumed session ABI (see swift/README.md).

package com.cyberremesher.mobile

/** One sample along an input stroke, mirroring `CyberStrokeSample`. */
data class StrokeSample(
    val x: Double,
    val y: Double,
    val pressure: Double = 1.0,
    val altitudeAngle: Double = Math.PI / 2,
    val azimuthAngle: Double = 0.0,
    val timestamp: Double = 0.0,
)

/** Thrown when a `cyber_*` call returns a non-OK status. */
class CyberException(val status: Int, message: String) : RuntimeException(message)

/**
 * Static facade over the engine's C ABI. All methods that can fail throw
 * [CyberException] carrying the ABI's thread-local last-error string.
 *
 * Handles (`meshHandle`, `sessionHandle`) are native pointers boxed as `Long`;
 * `0L` means null. Callers must pair every create with a destroy.
 */
object CyberEngine {
    init {
        System.loadLibrary("cyber_shell") // built by cpp/CMakeLists.txt
    }

    /** Human-readable engine version, e.g. "1.0.0". */
    external fun version(): String

    /** Numeric ABI version: (major shl 16) or minor. */
    external fun abiVersion(): Int

    // --- Mesh -------------------------------------------------------------
    /** Loads a mesh from a file; returns a native mesh handle (never 0 on ok). */
    @Throws(CyberException::class)
    external fun meshLoad(path: String): Long

    /** Saves a mesh to a file (format inferred from the extension). */
    @Throws(CyberException::class)
    external fun meshSave(meshHandle: Long, path: String)

    external fun meshVertexCount(meshHandle: Long): Long
    external fun meshFaceCount(meshHandle: Long): Long
    external fun meshDestroy(meshHandle: Long)

    // --- Session ----------------------------------------------------------
    /** Opens an editing session over a mesh; returns a native session handle. */
    @Throws(CyberException::class)
    external fun sessionCreate(meshHandle: Long): Long

    external fun sessionDestroy(sessionHandle: Long)

    /** Returns an owned snapshot mesh handle of the session's live edit mesh. */
    @Throws(CyberException::class)
    external fun sessionSnapshot(sessionHandle: Long): Long

    /**
     * Injects a completed stroke. Samples are passed as parallel primitive
     * arrays to avoid per-sample JNI object marshaling on the hot path.
     */
    @Throws(CyberException::class)
    external fun sessionInjectStroke(
        sessionHandle: Long,
        xs: DoubleArray,
        ys: DoubleArray,
        pressures: DoubleArray,
        altitudes: DoubleArray,
        azimuths: DoubleArray,
        timestamps: DoubleArray,
    )

    @Throws(CyberException::class)
    external fun sessionInjectTap(sessionHandle: Long, x: Double, y: Double)

    @Throws(CyberException::class)
    external fun sessionInjectChord(sessionHandle: Long, buttons: LongArray)

    // --- Render surface (Vulkan/GL host) ----------------------------------
    /**
     * Attaches an Android `Surface` (an `android.view.Surface`) to the engine
     * render backend. Native derives an `ANativeWindow` and hands it to the
     * Vulkan swapchain. Assumed ABI extension (`cyber_session_attach_android_surface`).
     */
    @Throws(CyberException::class)
    external fun sessionAttachSurface(sessionHandle: Long, surface: Any)

    @Throws(CyberException::class)
    external fun sessionSetDrawableSize(sessionHandle: Long, width: Int, height: Int)

    external fun sessionDetachSurface(sessionHandle: Long)

    // --- Remesh (long op) -------------------------------------------------
    /**
     * Runs the blocking remesh. [progress] receives fraction 0..1; [cancel]
     * returns true to request cooperative cancel (polled by the engine). Runs
     * on the calling thread — call it from a background dispatcher.
     * Returns the new mesh handle.
     */
    @Throws(CyberException::class)
    external fun remesh(
        meshHandle: Long,
        targetQuads: Int,
        pureQuad: Boolean,
        preserveSharp: Boolean,
        sharpAngleDegrees: Double,
        progress: (Double) -> Unit,
        cancel: () -> Boolean,
    ): Long
}
