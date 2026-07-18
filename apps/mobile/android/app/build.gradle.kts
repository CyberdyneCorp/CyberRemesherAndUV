// UNVERIFIED: Android shell app module build script (task 8.5). Not built in CI;
// requires the Android SDK/NDK and a cross-compiled libcyber_capi.
import java.io.File

plugins {
    id("com.android.application")
    id("org.jetbrains.kotlin.android")
    id("org.jetbrains.kotlin.plugin.serialization")
}

val cyberCapiDir: String = (project.findProperty("cyberCapiDir") as String?)
    ?: "../../../../build-android"

android {
    namespace = "com.cyberremesher.mobile"
    compileSdk = 34

    defaultConfig {
        applicationId = "com.cyberremesher.mobile"
        minSdk = 26
        targetSdk = 34
        versionCode = 1
        versionName = "1.0.0"
        ndk {
            abiFilters += listOf("arm64-v8a", "x86_64")
        }
        externalNativeBuild {
            cmake {
                // Forward the capi location to the app's NDK CMake script.
                arguments += "-DCYBER_CAPI_DIR=${File(rootDir, cyberCapiDir).absolutePath}"
                cppFlags += "-std=c++20"
            }
        }
    }

    externalNativeBuild {
        cmake {
            path = file("src/main/cpp/CMakeLists.txt")
            version = "3.22.1"
        }
    }

    buildFeatures {
        compose = true
    }
    composeOptions {
        kotlinCompilerExtensionVersion = "1.5.14"
    }

    compileOptions {
        sourceCompatibility = JavaVersion.VERSION_17
        targetCompatibility = JavaVersion.VERSION_17
    }
    kotlinOptions {
        jvmTarget = "17"
    }

    sourceSets["main"].java.srcDir("src/main/kotlin")
}

// Copy the cross-platform shared UI model (task 8.6) into the app's assets so
// both shells load byte-identical stage/toolbar/tutorial data.
val copySharedUiModel by tasks.registering(Copy::class) {
    from("../../shared") {
        include("stages.json", "toolbar.default.json", "tutorial.json")
    }
    into("src/main/assets")
}
tasks.named("preBuild") { dependsOn(copySharedUiModel) }

dependencies {
    implementation("androidx.core:core-ktx:1.13.1")
    implementation("androidx.appcompat:appcompat:1.7.0")
    implementation("androidx.activity:activity-compose:1.9.0")
    implementation("androidx.lifecycle:lifecycle-runtime-ktx:2.8.3")
    implementation(platform("androidx.compose:compose-bom:2024.06.00"))
    implementation("androidx.compose.ui:ui")
    implementation("androidx.compose.material3:material3")
    implementation("org.jetbrains.kotlinx:kotlinx-serialization-json:1.7.1")
    implementation("org.jetbrains.kotlinx:kotlinx-coroutines-android:1.8.1")
}
