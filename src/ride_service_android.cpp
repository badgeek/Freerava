// See ride_service_android.h. Starts/stops the one Java component in the whole
// app — a foreground Service — over JNI. Same PushLocalFrame / FindClass /
// swallow-exception dance as camera_android.cpp and gps_telemetry_android.cpp.
#if defined(__ANDROID__)
#include "ride_service_android.h"

#include "android_env.h"

#include <android/log.h>
#include <jni.h>

#define SVC_LOG(...) __android_log_print(ANDROID_LOG_INFO, "cyclomp-ride", __VA_ARGS__)

namespace ridesvc {
namespace {

constexpr const char *kNotifPerm = "android.permission.POST_NOTIFICATIONS";
constexpr const char *kServiceClass = "dev.bauhouse.cyclomp.RideService";

JNIEnv *env() {
    JavaVM *vm = cyclomp_java_vm();
    if (!vm) return nullptr;
    JNIEnv *e = nullptr;
    if (vm->GetEnv(reinterpret_cast<void **>(&e), JNI_VERSION_1_6) == JNI_OK) return e;
    return vm->AttachCurrentThread(&e, nullptr) == JNI_OK ? e : nullptr;
}

// Swallow and log a pending JNI exception; true when one was cleared.
bool cleared(JNIEnv *e) {
    if (!e->ExceptionCheck()) return false;
    e->ExceptionDescribe();
    e->ExceptionClear();
    return true;
}

// Build an explicit Intent targeting RideService by name.
//
// We deliberately do NOT FindClass("dev/bauhouse/cyclomp/RideService"): this
// runs on the NDK's android_main thread, whose FindClass resolves against the
// BOOTSTRAP classloader (system libs only, no app dex) — framework classes
// like Intent resolve fine, but our own app class throws ClassNotFoundException
// and JNI aborts the process. Intent.setClassName(Context, String) names the
// component with a plain string and touches only framework classes.
jobject make_intent(JNIEnv *e, jobject activity) {
    jclass intentCls = e->FindClass("android/content/Intent");
    if (!intentCls) { cleared(e); return nullptr; }
    jmethodID ctor = e->GetMethodID(intentCls, "<init>", "()V");
    jobject intent = ctor ? e->NewObject(intentCls, ctor) : nullptr;
    if (cleared(e) || !intent) return nullptr;
    jmethodID setClassName = e->GetMethodID(
        intentCls, "setClassName",
        "(Landroid/content/Context;Ljava/lang/String;)Landroid/content/Intent;");
    if (!setClassName) { cleared(e); return nullptr; }
    e->CallObjectMethod(intent, setClassName, activity,
                        e->NewStringUTF(kServiceClass));
    return cleared(e) ? nullptr : intent;
}

bool notif_granted(JNIEnv *e, jobject activity) {
    jclass ctx = e->FindClass("android/content/Context");
    jmethodID check = ctx ? e->GetMethodID(ctx, "checkSelfPermission",
                                           "(Ljava/lang/String;)I")
                          : nullptr;
    if (!check) { cleared(e); return true; } // can't check: don't block the ride
    jint r = e->CallIntMethod(activity, check, e->NewStringUTF(kNotifPerm));
    return !cleared(e) && r == 0; // PackageManager.PERMISSION_GRANTED
}

bool g_notif_requested = false;

} // namespace

void request_notification_permission() {
    if (g_notif_requested) return;
    JNIEnv *e = env();
    jobject activity = cyclomp_activity();
    if (!e || !activity) return;
    e->PushLocalFrame(8);
    // checkSelfPermission returns GRANTED on API < 33 (POST_NOTIFICATIONS is not
    // a runtime permission there), so this whole block no-ops on older devices.
    if (!notif_granted(e, activity)) {
        g_notif_requested = true;
        jclass activityCls = e->FindClass("android/app/Activity");
        jmethodID req = activityCls
                            ? e->GetMethodID(activityCls, "requestPermissions",
                                             "([Ljava/lang/String;I)V")
                            : nullptr;
        jclass stringCls = e->FindClass("java/lang/String");
        if (req && stringCls) {
            jobjectArray perms = e->NewObjectArray(1, stringCls, nullptr);
            e->SetObjectArrayElement(perms, 0, e->NewStringUTF(kNotifPerm));
            e->CallVoidMethod(activity, req, perms, 3); // 1=location, 2=camera
            if (!cleared(e)) SVC_LOG("requested POST_NOTIFICATIONS");
        }
    }
    cleared(e);
    e->PopLocalFrame(nullptr);
}

void start() {
    JNIEnv *e = env();
    jobject activity = cyclomp_activity();
    if (!e || !activity) return;
    e->PushLocalFrame(8);
    jobject intent = make_intent(e, activity);
    if (intent) {
        jclass ctxCls = e->FindClass("android/content/Context");
        // startForegroundService exists from API 26 — our minSdk.
        jmethodID start =
            ctxCls ? e->GetMethodID(
                         ctxCls, "startForegroundService",
                         "(Landroid/content/Intent;)Landroid/content/ComponentName;")
                   : nullptr;
        if (start) {
            e->CallObjectMethod(activity, start, intent);
            if (!cleared(e)) SVC_LOG("ride service started");
        }
    } else {
        SVC_LOG("ride service: intent build failed");
    }
    cleared(e);
    e->PopLocalFrame(nullptr);
}

void stop() {
    JNIEnv *e = env();
    jobject activity = cyclomp_activity();
    if (!e || !activity) return;
    e->PushLocalFrame(8);
    jobject intent = make_intent(e, activity);
    if (intent) {
        jclass ctxCls = e->FindClass("android/content/Context");
        jmethodID stopSvc =
            ctxCls ? e->GetMethodID(ctxCls, "stopService",
                                    "(Landroid/content/Intent;)Z")
                   : nullptr;
        if (stopSvc) {
            e->CallBooleanMethod(activity, stopSvc, intent);
            if (!cleared(e)) SVC_LOG("ride service stopped");
        }
    }
    cleared(e);
    e->PopLocalFrame(nullptr);
}

} // namespace ridesvc
#endif
