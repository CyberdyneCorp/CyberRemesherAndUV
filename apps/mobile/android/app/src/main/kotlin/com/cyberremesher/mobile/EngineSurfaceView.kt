// UNVERIFIED: Android shell — Vulkan/GL SurfaceView host (task 8.5). Not built
// in CI. A SurfaceView whose Surface is handed to native (ANativeWindow) and
// attached to the engine render backend (Vulkan on Android per the design; GL
// is a fallback only). Also owns the touch feed for the viewport.

package com.cyberremesher.mobile

import android.content.Context
import android.view.MotionEvent
import android.view.SurfaceHolder
import android.view.SurfaceView

/**
 * The engine viewport surface. Rendering is driven natively; this class only
 * manages the Surface lifecycle and forwards touches.
 */
class EngineSurfaceView(context: Context, private val session: EngineSession) :
    SurfaceView(context), SurfaceHolder.Callback {

    private val forwarder = TouchInputForwarder(session)

    init {
        holder.addCallback(this)
        isFocusable = true
        isFocusableInTouchMode = true
    }

    override fun surfaceCreated(holder: SurfaceHolder) {
        val session = this.session.sessionHandle
        if (session == 0L) return
        try {
            CyberEngine.sessionAttachSurface(session, holder.surface)
        } catch (e: CyberException) {
            this.session.log.error("surface attach failed: ${e.message}")
        }
    }

    override fun surfaceChanged(holder: SurfaceHolder, format: Int, width: Int, height: Int) {
        val session = this.session.sessionHandle
        if (session == 0L) return
        try {
            CyberEngine.sessionSetDrawableSize(session, width, height)
        } catch (e: CyberException) {
            this.session.log.error("set drawable size failed: ${e.message}")
        }
    }

    override fun surfaceDestroyed(holder: SurfaceHolder) {
        val session = this.session.sessionHandle
        if (session != 0L) CyberEngine.sessionDetachSurface(session)
    }

    override fun onTouchEvent(event: MotionEvent): Boolean {
        forwarder.onMotionEvent(event, width, height)
        return true
    }
}
