// UNVERIFIED: Android shell — decoders for the shared data-driven UI model
// (task 8.6). Not built in CI. Mirrors ../../shared/stages.json and
// toolbar.default.json (same files the iPadOS shell decodes). Loaded from the
// app's assets/ (copied there by the Gradle `copySharedUiModel` task).

package com.cyberremesher.mobile.ui

import android.content.Context
import kotlinx.serialization.SerialName
import kotlinx.serialization.Serializable
import kotlinx.serialization.json.Json

@Serializable
data class Stage(
    val id: String,
    val title: String,
    @SerialName("androidIcon") val androidIcon: String = "",
    val summary: String = "",
)

@Serializable
data class ActionDescriptor(
    val id: String,
    val title: String,
    @SerialName("androidIcon") val androidIcon: String = "",
    val stages: List<String> = emptyList(),
    val command: String,
) {
    /** True if this action is available in [stageId] (empty stages = all). */
    fun availableIn(stageId: String): Boolean =
        stages.isEmpty() || stages.contains(stageId)
}

@Serializable
data class StageModel(val schemaVersion: Int = 1, val stages: List<Stage> = emptyList())

@Serializable
data class ToolbarModel(
    val schemaVersion: Int = 1,
    val actions: List<ActionDescriptor> = emptyList(),
    val defaultToolbar: List<String> = emptyList(),
) {
    /** The default toolbar resolved to descriptors, in order. */
    fun defaultActions(): List<ActionDescriptor> {
        val byId = actions.associateBy { it.id }
        return defaultToolbar.mapNotNull { byId[it] }
    }
}

/** Loads the shared UI model from assets, falling back to a minimal built-in. */
object UiModelLoader {
    private val json = Json { ignoreUnknownKeys = true }

    fun loadStages(context: Context): StageModel =
        decode(context, "stages.json") ?: StageModel(
            stages = listOf(
                Stage("retopology", "Retopology"),
                Stage("uv", "UV"),
                Stage("baking", "Baking"),
            )
        )

    fun loadToolbar(context: Context): ToolbarModel =
        decode(context, "toolbar.default.json") ?: ToolbarModel()

    private inline fun <reified T> decode(context: Context, asset: String): T? =
        runCatching {
            context.assets.open(asset).bufferedReader().use { reader ->
                json.decodeFromString<T>(reader.readText())
            }
        }.getOrNull()
}
