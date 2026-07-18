// UNVERIFIED: Android shell — session/document model (tasks 8.5 + 8.6). Not
// built in CI. Owns the native session/mesh handles, the lifecycle-autosave
// hook (8.5), the in-app log, and the long-op state machine with atomic commit
// (8.6). Kotlin-only logic; the render/input wiring lives in the views.

package com.cyberremesher.mobile

import androidx.compose.runtime.getValue
import androidx.compose.runtime.mutableStateOf
import androidx.compose.runtime.setValue
import com.cyberremesher.mobile.ui.LogBuffer
import kotlinx.coroutines.CoroutineScope
import kotlinx.coroutines.Dispatchers
import kotlinx.coroutines.Job
import kotlinx.coroutines.launch
import kotlinx.coroutines.withContext
import java.io.File
import java.util.concurrent.atomic.AtomicBoolean

/** The long-operation UI state (8.6). Modeled so the commit is atomic. */
sealed interface OperationState {
    data object Idle : OperationState
    data class Running(val progress: Double, val label: String) : OperationState
    data object Cancelling : OperationState
}

/**
 * A single-document editing session. Compose observes [operation], [stageId]
 * and the log; the surface/input views read [sessionHandle].
 */
class EngineSession(
    private val scope: CoroutineScope,
    private val autosaveFile: File,
    val log: LogBuffer,
) {
    var sessionHandle: Long = 0L
        private set
    private var meshHandle: Long = 0L

    var operation by mutableStateOf<OperationState>(OperationState.Idle)
        private set

    private var cancelRequested = AtomicBoolean(false)
    private var runningJob: Job? = null

    init {
        log.info("shell ready — engine ${CyberEngine.version()}")
    }

    /** Opens a document over a mesh file. */
    fun open(path: String) {
        try {
            val mesh = CyberEngine.meshLoad(path)
            val session = CyberEngine.sessionCreate(mesh)
            replace(mesh = mesh, session = session)
            log.info("opened document (${CyberEngine.meshVertexCount(mesh)} verts)")
        } catch (e: CyberException) {
            log.error("open failed: ${e.message}")
        }
    }

    // --- Lifecycle autosave (8.5) ----------------------------------------

    /**
     * Called from ON_PAUSE/ON_STOP. Must be quick and synchronous: the process
     * may be frozen or killed immediately after.
     */
    fun autosaveOnStop() {
        val session = sessionHandle
        if (session == 0L) return
        try {
            val snapshot = CyberEngine.sessionSnapshot(session)
            try {
                CyberEngine.meshSave(snapshot, autosaveFile.absolutePath)
                log.info("autosaved to ${autosaveFile.name}")
            } finally {
                CyberEngine.meshDestroy(snapshot)
            }
        } catch (e: CyberException) {
            log.error("autosave failed: ${e.message}")
        }
    }

    // --- Long op with atomic commit (8.6) --------------------------------

    /**
     * Runs a remesh off the main thread. The current document stays displayed
     * for the whole run; the new session is built first and swapped in one step
     * only on success. Cancel leaves the document untouched (no stale flash).
     */
    fun runRemesh(targetQuads: Int, pureQuad: Boolean) {
        if (operation != OperationState.Idle || meshHandle == 0L) return
        val source = meshHandle
        cancelRequested.set(false)
        operation = OperationState.Running(progress = 0.0, label = "Remeshing")
        log.info("remesh started (target $targetQuads quads)")

        runningJob = scope.launch {
            val result = runCatching {
                withContext(Dispatchers.Default) {
                    CyberEngine.remesh(
                        meshHandle = source,
                        targetQuads = targetQuads,
                        pureQuad = pureQuad,
                        preserveSharp = true,
                        sharpAngleDegrees = 30.0,
                        progress = { p -> onProgress(p) },
                        cancel = { cancelRequested.get() },
                    )
                }
            }
            result.onSuccess { newMesh ->
                // Atomic commit: build the new session before publishing.
                try {
                    val newSession = CyberEngine.sessionCreate(newMesh)
                    replace(mesh = newMesh, session = newSession)
                    operation = OperationState.Idle
                    log.info("remesh committed (${CyberEngine.meshFaceCount(newMesh)} faces)")
                } catch (e: CyberException) {
                    CyberEngine.meshDestroy(newMesh)
                    rollback("commit failed: ${e.message}")
                }
            }.onFailure { e ->
                rollback(if (cancelRequested.get()) "cancelled" else (e.message ?: "error"))
            }
        }
    }

    fun cancelOperation() {
        if (operation !is OperationState.Running) return
        operation = OperationState.Cancelling
        cancelRequested.set(true) // polled by the engine's cancel callback
        log.info("cancel requested")
    }

    private fun onProgress(value: Double) {
        val current = operation
        if (current is OperationState.Running) {
            operation = current.copy(progress = value)
        }
    }

    private fun rollback(reason: String) {
        operation = OperationState.Idle
        log.warn("remesh ended: $reason (document unchanged)")
    }

    // --- Handle ownership -------------------------------------------------

    private fun replace(mesh: Long, session: Long) {
        val oldMesh = meshHandle
        val oldSession = sessionHandle
        meshHandle = mesh
        sessionHandle = session
        if (oldSession != 0L) CyberEngine.sessionDestroy(oldSession)
        if (oldMesh != 0L) CyberEngine.meshDestroy(oldMesh)
    }

    fun dispose() {
        runningJob?.cancel()
        replace(mesh = 0L, session = 0L)
    }
}
