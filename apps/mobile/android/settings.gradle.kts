// UNVERIFIED: Android shell settings (task 8.5). Not built in CI; not wired into
// the repo CMake. Requires the Android SDK/NDK + a Gradle wrapper.
pluginManagement {
    repositories {
        google()
        mavenCentral()
        gradlePluginPortal()
    }
}

dependencyResolutionManagement {
    repositoriesMode.set(RepositoriesMode.FAIL_ON_PROJECT_REPOS)
    repositories {
        google()
        mavenCentral()
    }
}

rootProject.name = "CyberRemesher"
include(":app")
