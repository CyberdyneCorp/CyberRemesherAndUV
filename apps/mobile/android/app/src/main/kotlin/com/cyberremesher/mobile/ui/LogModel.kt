// UNVERIFIED: Android shell — in-app log model (task 8.6, quiet by default).
// Not built in CI. A bounded, Compose-observable ring of leveled entries. Only
// INFO+ show unless the user enables verbose (DEBUG).

package com.cyberremesher.mobile.ui

import androidx.compose.runtime.mutableStateListOf

enum class LogLevel(val label: String) {
    DEBUG("DEBUG"), INFO("INFO"), WARN("WARN"), ERROR("ERROR")
}

data class LogEntry(val timeMs: Long, val level: LogLevel, val message: String)

/** Bounded, observable log buffer. */
class LogBuffer(private val capacity: Int = 500) {
    /** Backing list Compose can observe. */
    val entries = mutableStateListOf<LogEntry>()

    fun debug(m: String) = append(LogLevel.DEBUG, m)
    fun info(m: String) = append(LogLevel.INFO, m)
    fun warn(m: String) = append(LogLevel.WARN, m)
    fun error(m: String) = append(LogLevel.ERROR, m)

    /** Entries at or above [minimum], newest last. */
    fun visible(minimum: LogLevel): List<LogEntry> =
        entries.filter { it.level.ordinal >= minimum.ordinal }

    private fun append(level: LogLevel, message: String) {
        entries.add(LogEntry(System.currentTimeMillis(), level, message))
        while (entries.size > capacity) entries.removeAt(0)
    }
}
