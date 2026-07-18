// UNVERIFIED: Android shell — JNI shim over the C ABI (task 8.5). Not built in
// CI; compiled by the app's own NDK CMake (cpp/CMakeLists.txt), NOT the repo
// CMake. Bridges the Kotlin `CyberEngine` externals to `cyber_capi.h`. Targets
// the assumed session ABI documented in swift/README.md; if capi lands with
// different spellings, only this file changes.
//
// Note: handles cross the boundary as jlong (reinterpreted pointers). This file
// has never been compiled — treat symbol names/signatures as the contract to
// reconcile against the real capi header on first Android build.

#include <jni.h>
#include <android/native_window.h>
#include <android/native_window_jni.h>

#include <cstdint>
#include <string>
#include <vector>

extern "C" {
#include "cyber_capi.h"
}

// --- assumed session ABI (mirrors swift/README.md) -----------------------
// These are declared here (not in the shipped cyber_capi.h yet) so the shim
// compiles against the assumed contract. Remove once capi exports them.
extern "C" {
typedef struct CyberSession CyberSession;
typedef struct CyberStrokeSample {
    double x, y, pressure, altitude_angle, azimuth_angle, timestamp;
} CyberStrokeSample;

const char* cyber_version_string(void);
int32_t cyber_abi_version(void);
const char* cyber_last_error_message(void);

int cyber_session_create(const CyberMesh* mesh, CyberSession** out);
void cyber_session_destroy(CyberSession* session);
int cyber_session_snapshot_mesh(const CyberSession* session, CyberMesh** out);
int cyber_session_inject_stroke(CyberSession* session, const CyberStrokeSample* samples,
                                size_t count);
int cyber_session_inject_tap(CyberSession* session, double x, double y);
int cyber_session_inject_chord(CyberSession* session, const uint64_t* buttons, size_t count);
int cyber_session_attach_android_surface(CyberSession* session, ANativeWindow* window);
int cyber_session_set_drawable_size(CyberSession* session, double w, double h);
int cyber_session_detach_surface(CyberSession* session);
}

namespace {

// Throws a Kotlin CyberException(status, message) and returns from the caller.
void throwCyber(JNIEnv* env, int status) {
    const char* msg = cyber_last_error_message();
    jclass cls = env->FindClass("com/cyberremesher/mobile/CyberException");
    if (cls == nullptr) {
        return;
    }
    jmethodID ctor = env->GetMethodID(cls, "<init>", "(ILjava/lang/String;)V");
    jstring jmsg = env->NewStringUTF(msg != nullptr ? msg : "");
    auto* ex = static_cast<jthrowable>(
        env->NewObject(cls, ctor, static_cast<jint>(status), jmsg));
    env->Throw(ex);
}

// Reinterpret helpers keep the pointer<->jlong casts in one place.
CyberMesh* mesh(jlong h) { return reinterpret_cast<CyberMesh*>(h); }
CyberSession* session(jlong h) { return reinterpret_cast<CyberSession*>(h); }
jlong box(void* p) { return reinterpret_cast<jlong>(p); }

// Bridges the remesh C callbacks to Kotlin lambdas (Function1<Double,Unit> and
// Function0<Boolean>) for the duration of one blocking remesh call.
struct RemeshBridge {
    JNIEnv* env;
    jobject progress; // kotlin.jvm.functions.Function1
    jobject cancel;   // kotlin.jvm.functions.Function0
    jmethodID progressInvoke;
    jmethodID cancelInvoke;
    jclass doubleCls;
    jmethodID doubleCtor;
    jclass booleanCls;
    jmethodID booleanValue;
};

void progressTrampoline(float fraction, const char* /*stage*/, void* user) {
    auto* b = static_cast<RemeshBridge*>(user);
    jobject boxed = b->env->NewObject(b->doubleCls, b->doubleCtor,
                                      static_cast<jdouble>(fraction));
    b->env->CallObjectMethod(b->progress, b->progressInvoke, boxed);
    b->env->DeleteLocalRef(boxed);
}

int cancelTrampoline(void* user) {
    auto* b = static_cast<RemeshBridge*>(user);
    jobject r = b->env->CallObjectMethod(b->cancel, b->cancelInvoke);
    jboolean requested = b->env->CallBooleanMethod(r, b->booleanValue);
    b->env->DeleteLocalRef(r);
    return requested == JNI_TRUE ? 1 : 0;
}

} // namespace

extern "C" {

JNIEXPORT jstring JNICALL
Java_com_cyberremesher_mobile_CyberEngine_version(JNIEnv* env, jobject) {
    return env->NewStringUTF(cyber_version_string());
}

JNIEXPORT jint JNICALL
Java_com_cyberremesher_mobile_CyberEngine_abiVersion(JNIEnv*, jobject) {
    return static_cast<jint>(cyber_abi_version());
}

JNIEXPORT jlong JNICALL
Java_com_cyberremesher_mobile_CyberEngine_meshLoad(JNIEnv* env, jobject, jstring path) {
    const char* cpath = env->GetStringUTFChars(path, nullptr);
    CyberMesh* out = nullptr;
    int status = cyber_mesh_load_obj(cpath, &out);
    env->ReleaseStringUTFChars(path, cpath);
    if (status != CYBER_OK) {
        throwCyber(env, status);
        return 0;
    }
    return box(out);
}

JNIEXPORT void JNICALL
Java_com_cyberremesher_mobile_CyberEngine_meshSave(JNIEnv* env, jobject, jlong h, jstring path) {
    const char* cpath = env->GetStringUTFChars(path, nullptr);
    int status = cyber_mesh_save_obj(mesh(h), cpath);
    env->ReleaseStringUTFChars(path, cpath);
    if (status != CYBER_OK) {
        throwCyber(env, status);
    }
}

JNIEXPORT jlong JNICALL
Java_com_cyberremesher_mobile_CyberEngine_meshVertexCount(JNIEnv*, jobject, jlong h) {
    return static_cast<jlong>(cyber_mesh_vertex_count(mesh(h)));
}

JNIEXPORT jlong JNICALL
Java_com_cyberremesher_mobile_CyberEngine_meshFaceCount(JNIEnv*, jobject, jlong h) {
    return static_cast<jlong>(cyber_mesh_face_count(mesh(h)));
}

JNIEXPORT void JNICALL
Java_com_cyberremesher_mobile_CyberEngine_meshDestroy(JNIEnv*, jobject, jlong h) {
    cyber_mesh_free(mesh(h));
}

JNIEXPORT jlong JNICALL
Java_com_cyberremesher_mobile_CyberEngine_sessionCreate(JNIEnv* env, jobject, jlong meshH) {
    CyberSession* out = nullptr;
    int status = cyber_session_create(mesh(meshH), &out);
    if (status != CYBER_OK) {
        throwCyber(env, status);
        return 0;
    }
    return box(out);
}

JNIEXPORT void JNICALL
Java_com_cyberremesher_mobile_CyberEngine_sessionDestroy(JNIEnv*, jobject, jlong h) {
    cyber_session_destroy(session(h));
}

JNIEXPORT jlong JNICALL
Java_com_cyberremesher_mobile_CyberEngine_sessionSnapshot(JNIEnv* env, jobject, jlong h) {
    CyberMesh* out = nullptr;
    int status = cyber_session_snapshot_mesh(session(h), &out);
    if (status != CYBER_OK) {
        throwCyber(env, status);
        return 0;
    }
    return box(out);
}

JNIEXPORT void JNICALL
Java_com_cyberremesher_mobile_CyberEngine_sessionInjectStroke(
    JNIEnv* env, jobject, jlong h, jdoubleArray xs, jdoubleArray ys, jdoubleArray pressures,
    jdoubleArray altitudes, jdoubleArray azimuths, jdoubleArray timestamps) {
    const jsize count = env->GetArrayLength(xs);
    std::vector<CyberStrokeSample> samples(static_cast<size_t>(count));

    jdouble* px = env->GetDoubleArrayElements(xs, nullptr);
    jdouble* py = env->GetDoubleArrayElements(ys, nullptr);
    jdouble* pp = env->GetDoubleArrayElements(pressures, nullptr);
    jdouble* pa = env->GetDoubleArrayElements(altitudes, nullptr);
    jdouble* pz = env->GetDoubleArrayElements(azimuths, nullptr);
    jdouble* pt = env->GetDoubleArrayElements(timestamps, nullptr);

    for (jsize i = 0; i < count; ++i) {
        const auto k = static_cast<size_t>(i);
        samples[k] = CyberStrokeSample{px[i], py[i], pp[i], pa[i], pz[i], pt[i]};
    }

    int status = cyber_session_inject_stroke(session(h), samples.data(), samples.size());

    env->ReleaseDoubleArrayElements(xs, px, JNI_ABORT);
    env->ReleaseDoubleArrayElements(ys, py, JNI_ABORT);
    env->ReleaseDoubleArrayElements(pressures, pp, JNI_ABORT);
    env->ReleaseDoubleArrayElements(altitudes, pa, JNI_ABORT);
    env->ReleaseDoubleArrayElements(azimuths, pz, JNI_ABORT);
    env->ReleaseDoubleArrayElements(timestamps, pt, JNI_ABORT);

    if (status != CYBER_OK) {
        throwCyber(env, status);
    }
}

JNIEXPORT void JNICALL
Java_com_cyberremesher_mobile_CyberEngine_sessionInjectTap(
    JNIEnv* env, jobject, jlong h, jdouble x, jdouble y) {
    int status = cyber_session_inject_tap(session(h), x, y);
    if (status != CYBER_OK) {
        throwCyber(env, status);
    }
}

JNIEXPORT void JNICALL
Java_com_cyberremesher_mobile_CyberEngine_sessionInjectChord(
    JNIEnv* env, jobject, jlong h, jlongArray buttons) {
    const jsize count = env->GetArrayLength(buttons);
    jlong* raw = env->GetLongArrayElements(buttons, nullptr);
    std::vector<uint64_t> codes(static_cast<size_t>(count));
    for (jsize i = 0; i < count; ++i) {
        codes[static_cast<size_t>(i)] = static_cast<uint64_t>(raw[i]);
    }
    int status = cyber_session_inject_chord(session(h), codes.data(), codes.size());
    env->ReleaseLongArrayElements(buttons, raw, JNI_ABORT);
    if (status != CYBER_OK) {
        throwCyber(env, status);
    }
}

JNIEXPORT void JNICALL
Java_com_cyberremesher_mobile_CyberEngine_sessionAttachSurface(
    JNIEnv* env, jobject, jlong h, jobject surface) {
    ANativeWindow* window = ANativeWindow_fromSurface(env, surface);
    int status = cyber_session_attach_android_surface(session(h), window);
    // The engine retains the window; release our local reference either way.
    if (window != nullptr) {
        ANativeWindow_release(window);
    }
    if (status != CYBER_OK) {
        throwCyber(env, status);
    }
}

JNIEXPORT void JNICALL
Java_com_cyberremesher_mobile_CyberEngine_sessionSetDrawableSize(
    JNIEnv* env, jobject, jlong h, jint width, jint height) {
    int status = cyber_session_set_drawable_size(session(h), static_cast<double>(width),
                                                 static_cast<double>(height));
    if (status != CYBER_OK) {
        throwCyber(env, status);
    }
}

JNIEXPORT void JNICALL
Java_com_cyberremesher_mobile_CyberEngine_sessionDetachSurface(JNIEnv*, jobject, jlong h) {
    cyber_session_detach_surface(session(h));
}

JNIEXPORT jlong JNICALL
Java_com_cyberremesher_mobile_CyberEngine_remesh(
    JNIEnv* env, jobject, jlong meshH, jint targetQuads, jboolean pureQuad,
    jboolean preserveSharp, jdouble sharpAngleDegrees, jobject progress, jobject cancel) {
    CyberRemeshParams params;
    cyber_default_params(&params);
    params.targetQuads = targetQuads;
    params.pureQuads = pureQuad == JNI_TRUE ? 1 : 0;
    params.sharpEdgeDegrees = static_cast<float>(sharpAngleDegrees);
    (void)preserveSharp;

    RemeshBridge bridge{};
    bridge.env = env;
    bridge.progress = progress;
    bridge.cancel = cancel;
    bridge.progressInvoke = env->GetMethodID(
        env->GetObjectClass(progress), "invoke", "(Ljava/lang/Object;)Ljava/lang/Object;");
    bridge.cancelInvoke = env->GetMethodID(
        env->GetObjectClass(cancel), "invoke", "()Ljava/lang/Object;");
    bridge.doubleCls = env->FindClass("java/lang/Double");
    bridge.doubleCtor = env->GetMethodID(bridge.doubleCls, "<init>", "(D)V");
    bridge.booleanCls = env->FindClass("java/lang/Boolean");
    bridge.booleanValue = env->GetMethodID(bridge.booleanCls, "booleanValue", "()Z");

    CyberMesh* out = nullptr;
    int status = cyber_remesh(mesh(meshH), &params, progressTrampoline, cancelTrampoline,
                              &bridge, &out);
    if (status != CYBER_OK) {
        throwCyber(env, status);
        return 0;
    }
    return box(out);
}

} // extern "C"
