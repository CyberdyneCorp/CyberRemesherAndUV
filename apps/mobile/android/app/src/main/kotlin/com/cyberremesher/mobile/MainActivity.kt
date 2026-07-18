// UNVERIFIED: Android shell — activity entry + lifecycle autosave (task 8.5).
// Not built in CI. Owns the EngineSession and translates the Activity lifecycle
// (ON_PAUSE/ON_STOP) into the engine's autosave hook.

package com.cyberremesher.mobile

import android.os.Bundle
import androidx.activity.ComponentActivity
import androidx.activity.compose.setContent
import androidx.lifecycle.Lifecycle
import androidx.lifecycle.LifecycleEventObserver
import androidx.lifecycle.lifecycleScope
import com.cyberremesher.mobile.ui.CyberRemesherScreen
import com.cyberremesher.mobile.ui.LogBuffer
import com.cyberremesher.mobile.ui.UiModelLoader
import java.io.File

class MainActivity : ComponentActivity() {

    private lateinit var session: EngineSession

    override fun onCreate(savedInstanceState: Bundle?) {
        super.onCreate(savedInstanceState)

        val autosaveFile = File(filesDir, "autosave.obj")
        session = EngineSession(
            scope = lifecycleScope,
            autosaveFile = autosaveFile,
            log = LogBuffer(),
        )

        // Backgrounding autosave (8.5): persist on ON_STOP, before the process
        // can be frozen or killed.
        lifecycle.addObserver(LifecycleEventObserver { _, event ->
            if (event == Lifecycle.Event.ON_STOP) {
                session.autosaveOnStop()
            }
        })

        val stages = UiModelLoader.loadStages(this).stages
        val toolbar = UiModelLoader.loadToolbar(this).defaultActions()

        setContent {
            CyberRemesherScreen(session = session, stages = stages, toolbar = toolbar)
        }
    }

    override fun onDestroy() {
        session.dispose()
        super.onDestroy()
    }
}
