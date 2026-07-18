// UNVERIFIED: Android shell — touch event feed (task 8.5). Not built in CI.
// Turns MotionEvents into engine strokes in normalized 0..1 viewport
// coordinates and injects them into the session. Makes no routing decisions;
// the engine decides navigate-vs-draw (chording).

package com.cyberremesher.mobile

import android.view.MotionEvent

/**
 * Accumulates a stroke from a gesture and injects it into [EngineSession] on
 * pointer-up. Coordinates are normalized against the view size passed on begin,
 * so the engine stays resolution-independent. Historical (batched) samples are
 * expanded so high-frequency input is not lost.
 */
class TouchInputForwarder(private val session: EngineSession) {

    private val xs = ArrayList<Double>()
    private val ys = ArrayList<Double>()
    private val pressures = ArrayList<Double>()
    private val altitudes = ArrayList<Double>()
    private val azimuths = ArrayList<Double>()
    private val timestamps = ArrayList<Double>()

    private var width = 1
    private var height = 1

    /** Feed a raw MotionEvent for a view of size [viewWidth] x [viewHeight]. */
    fun onMotionEvent(event: MotionEvent, viewWidth: Int, viewHeight: Int) {
        width = viewWidth.coerceAtLeast(1)
        height = viewHeight.coerceAtLeast(1)
        when (event.actionMasked) {
            MotionEvent.ACTION_DOWN -> {
                clear()
                appendWithHistory(event)
            }
            MotionEvent.ACTION_MOVE -> appendWithHistory(event)
            MotionEvent.ACTION_UP -> {
                appendWithHistory(event)
                flush()
            }
            MotionEvent.ACTION_CANCEL -> clear() // drop the in-flight stroke
        }
    }

    private fun appendWithHistory(event: MotionEvent) {
        // Batched historical positions first, then the current one.
        for (h in 0 until event.historySize) {
            append(event.getHistoricalX(h), event.getHistoricalY(h),
                event.getHistoricalPressure(h), event.getHistoricalEventTime(h))
        }
        append(event.x, event.y, event.pressure, event.eventTime)
    }

    private fun append(px: Float, py: Float, pressure: Float, eventTimeMs: Long) {
        xs.add((px / width).toDouble().coerceIn(0.0, 1.0))
        ys.add((py / height).toDouble().coerceIn(0.0, 1.0))
        pressures.add(pressure.toDouble().coerceIn(0.0, 1.0))
        altitudes.add(Math.PI / 2) // finger: perpendicular; a stylus would vary
        azimuths.add(0.0)
        timestamps.add(eventTimeMs / 1000.0)
    }

    private fun flush() {
        if (session.sessionHandle == 0L || xs.isEmpty()) {
            clear()
            return
        }
        try {
            CyberEngine.sessionInjectStroke(
                session.sessionHandle,
                xs.toDoubleArray(), ys.toDoubleArray(), pressures.toDoubleArray(),
                altitudes.toDoubleArray(), azimuths.toDoubleArray(),
                timestamps.toDoubleArray(),
            )
        } catch (e: CyberException) {
            session.log.error("stroke inject failed: ${e.message}")
        } finally {
            clear()
        }
    }

    private fun clear() {
        xs.clear(); ys.clear(); pressures.clear()
        altitudes.clear(); azimuths.clear(); timestamps.clear()
    }
}
