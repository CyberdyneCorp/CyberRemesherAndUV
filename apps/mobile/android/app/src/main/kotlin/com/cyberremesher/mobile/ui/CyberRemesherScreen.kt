// UNVERIFIED: Android shell — Compose UI (task 8.6). Not built in CI. Composes
// the stage switcher, the native viewport surface with the touch feed, the
// configurable Action toolbar, the long-op progress/cancel overlay (atomic
// commit), and the in-app log view. Presentation only; behaviour is in the
// engine + EngineSession.

package com.cyberremesher.mobile.ui

import androidx.compose.foundation.layout.Arrangement
import androidx.compose.foundation.layout.Box
import androidx.compose.foundation.layout.Column
import androidx.compose.foundation.layout.Row
import androidx.compose.foundation.layout.fillMaxSize
import androidx.compose.foundation.layout.fillMaxWidth
import androidx.compose.foundation.layout.padding
import androidx.compose.foundation.layout.width
import androidx.compose.foundation.lazy.LazyColumn
import androidx.compose.foundation.lazy.items
import androidx.compose.material3.Button
import androidx.compose.material3.LinearProgressIndicator
import androidx.compose.material3.OutlinedButton
import androidx.compose.material3.SegmentedButton
import androidx.compose.material3.SegmentedButtonDefaults
import androidx.compose.material3.SingleChoiceSegmentedButtonRow
import androidx.compose.material3.Surface
import androidx.compose.material3.Switch
import androidx.compose.material3.Text
import androidx.compose.runtime.Composable
import androidx.compose.runtime.getValue
import androidx.compose.runtime.mutableStateOf
import androidx.compose.runtime.remember
import androidx.compose.runtime.setValue
import androidx.compose.ui.Alignment
import androidx.compose.ui.Modifier
import androidx.compose.ui.unit.dp
import androidx.compose.ui.viewinterop.AndroidView
import com.cyberremesher.mobile.EngineSession
import com.cyberremesher.mobile.EngineSurfaceView
import com.cyberremesher.mobile.OperationState

@Composable
fun CyberRemesherScreen(
    session: EngineSession,
    stages: List<Stage>,
    toolbar: List<ActionDescriptor>,
) {
    var selectedStage by remember { mutableStateOf(stages.firstOrNull()?.id ?: "retopology") }
    var activeAction by remember { mutableStateOf<String?>(null) }
    var showLog by remember { mutableStateOf(false) }

    Column(Modifier.fillMaxSize()) {
        StageSwitcher(stages, selectedStage) { selectedStage = it }

        Box(Modifier.weight(1f).fillMaxWidth()) {
            // Native engine viewport (Vulkan/GL) with the touch feed built in.
            AndroidView(
                factory = { ctx -> EngineSurfaceView(ctx, session) },
                modifier = Modifier.fillMaxSize(),
            )
            OperationOverlay(session.operation) { session.cancelOperation() }
        }

        ActionToolbar(
            actions = toolbar.filter { it.availableIn(selectedStage) },
            active = activeAction,
            onSelect = { activeAction = it },
            onOpenLog = { showLog = true },
        )
    }

    if (showLog) {
        LogSheet(session.log) { showLog = false }
    }
}

@Composable
private fun StageSwitcher(stages: List<Stage>, selected: String, onSelect: (String) -> Unit) {
    SingleChoiceSegmentedButtonRow(Modifier.fillMaxWidth().padding(8.dp)) {
        stages.forEachIndexed { index, stage ->
            SegmentedButton(
                selected = stage.id == selected,
                onClick = { onSelect(stage.id) },
                shape = SegmentedButtonDefaults.itemShape(index, stages.size),
            ) { Text(stage.title) }
        }
    }
}

@Composable
private fun ActionToolbar(
    actions: List<ActionDescriptor>,
    active: String?,
    onSelect: (String) -> Unit,
    onOpenLog: () -> Unit,
) {
    Row(
        Modifier.fillMaxWidth().padding(8.dp),
        horizontalArrangement = Arrangement.spacedBy(8.dp),
    ) {
        actions.forEach { action ->
            if (action.id == active) {
                Button(onClick = { onSelect(action.id) }) { Text(action.title) }
            } else {
                OutlinedButton(onClick = { onSelect(action.id) }) { Text(action.title) }
            }
        }
        OutlinedButton(onClick = onOpenLog) { Text("Log") }
    }
}

@Composable
private fun OperationOverlay(state: OperationState, onCancel: () -> Unit) {
    if (state is OperationState.Idle) return
    Box(Modifier.fillMaxSize(), contentAlignment = Alignment.Center) {
        Surface(tonalElevation = 4.dp) {
            Column(
                Modifier.padding(24.dp),
                horizontalAlignment = Alignment.CenterHorizontally,
                verticalArrangement = Arrangement.spacedBy(12.dp),
            ) {
                when (state) {
                    is OperationState.Running -> {
                        Text(state.label)
                        LinearProgressIndicator(
                            progress = { state.progress.toFloat() },
                            modifier = Modifier.width(240.dp),
                        )
                        Text("${(state.progress * 100).toInt()}%")
                        OutlinedButton(onClick = onCancel) { Text("Cancel") }
                    }
                    OperationState.Cancelling -> Text("Cancelling…")
                    OperationState.Idle -> Unit
                }
            }
        }
    }
}

@Composable
private fun LogSheet(log: LogBuffer, onClose: () -> Unit) {
    var verbose by remember { mutableStateOf(false) }
    val minimum = if (verbose) LogLevel.DEBUG else LogLevel.INFO
    Surface(Modifier.fillMaxSize()) {
        Column(Modifier.padding(12.dp)) {
            Row(
                Modifier.fillMaxWidth(),
                horizontalArrangement = Arrangement.spacedBy(8.dp),
                verticalAlignment = Alignment.CenterVertically,
            ) {
                Text("Log")
                Switch(checked = verbose, onCheckedChange = { verbose = it })
                Text("Verbose")
                OutlinedButton(onClick = onClose) { Text("Done") }
            }
            LazyColumn(Modifier.weight(1f)) {
                items(log.visible(minimum)) { entry ->
                    Text("${entry.level.label}  ${entry.message}")
                }
            }
        }
    }
}
