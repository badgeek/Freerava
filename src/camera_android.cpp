// See camera_android.h. NDK Camera2 (libcamera2ndk) + AImageReader, which are
// plain C APIs — this is how the camera is reachable at all under the project's
// zero-Java rule, the same escape hatch the compass uses with ASensorManager.
#if defined(__ANDROID__)
#include "camera_android.h"

#include "android_env.h"

#include <camera/NdkCameraCaptureSession.h>
#include <camera/NdkCameraDevice.h>
#include <camera/NdkCameraError.h>
#include <camera/NdkCameraManager.h>
#include <camera/NdkCameraMetadata.h>
#include <camera/NdkCameraMetadataTags.h>
#include <media/NdkImage.h>
#include <media/NdkImageReader.h>

#include <android/log.h>
#include <jni.h>

#include <condition_variable>
#include <cstdio>
#include <mutex>
#include <string>

#define CAM_LOG(...) __android_log_print(ANDROID_LOG_INFO, "cyclomp-camera", __VA_ARGS__)
#define CAM_ERR(...) __android_log_print(ANDROID_LOG_ERROR, "cyclomp-camera", __VA_ARGS__)

namespace selfie {
namespace {

constexpr const char *kPermission = "android.permission.CAMERA";

JNIEnv *env() {
    JavaVM *vm = cyclomp_java_vm();
    if (!vm) return nullptr;
    JNIEnv *e = nullptr;
    if (vm->GetEnv(reinterpret_cast<void **>(&e), JNI_VERSION_1_6) == JNI_OK) return e;
    return vm->AttachCurrentThread(&e, nullptr) == JNI_OK ? e : nullptr;
}

// Swallow and report a pending JNI exception; true when one was cleared.
bool cleared(JNIEnv *e) {
    if (!e->ExceptionCheck()) return false;
    e->ExceptionClear();
    return true;
}

bool g_requested = false;

// ---- capture plumbing -------------------------------------------------------
// Every Camera2 callback arrives on a HAL thread, so the whole exchange is
// funnelled through one mutex + condvar and the calling thread waits on it.
struct Pending {
    std::mutex m;
    std::condition_variable cv;
    bool done = false;
    bool ok = false;
    std::string error;
    std::string path;
    int width = 0, height = 0;

    void finish(bool good, std::string err) {
        {
            std::lock_guard<std::mutex> lk(m);
            if (done) return; // first outcome wins; later callbacks are noise
            done = true; ok = good; error = std::move(err);
        }
        cv.notify_all();
    }
};

// Write the single plane of a JPEG AImage straight to disk. The HAL hands us a
// finished JPEG, so there is nothing to encode here.
bool write_jpeg(AImage *img, const std::string &path, std::string *err) {
    uint8_t *data = nullptr;
    int len = 0;
    if (AImage_getPlaneData(img, 0, &data, &len) != AMEDIA_OK || !data || len <= 0) {
        *err = "empty JPEG plane";
        return false;
    }
    std::FILE *f = std::fopen(path.c_str(), "wb");
    if (!f) { *err = "cannot open " + path; return false; }
    const size_t wrote = std::fwrite(data, 1, (size_t)len, f);
    std::fclose(f);
    if (wrote != (size_t)len) { *err = "short write"; return false; }
    return true;
}

void on_image(void *ctx, AImageReader *reader) {
    auto *p = static_cast<Pending *>(ctx);
    AImage *img = nullptr;
    if (AImageReader_acquireLatestImage(reader, &img) != AMEDIA_OK || !img) {
        p->finish(false, "acquireLatestImage failed");
        return;
    }
    AImage_getWidth(img, &p->width);
    AImage_getHeight(img, &p->height);
    std::string err;
    const bool good = write_jpeg(img, p->path, &err);
    AImage_delete(img);
    p->finish(good, err);
}

// The session/device callbacks we must supply. Only the failure paths matter:
// success is observed through the image callback above.
void dev_disconnected(void *, ACameraDevice *) { CAM_LOG("camera disconnected"); }
void dev_error(void *ctx, ACameraDevice *, int err) {
    static_cast<Pending *>(ctx)->finish(false, "device error " + std::to_string(err));
}
void ses_closed(void *, ACameraCaptureSession *) {}
void ses_ready(void *, ACameraCaptureSession *) {}
void ses_active(void *, ACameraCaptureSession *) {}
void cap_failed(void *ctx, ACameraCaptureSession *, ACaptureRequest *,
                ACameraCaptureFailure *) {
    static_cast<Pending *>(ctx)->finish(false, "capture failed");
}
void cap_started(void *, ACameraCaptureSession *, const ACaptureRequest *, int64_t) {}
void cap_progressed(void *, ACameraCaptureSession *, ACaptureRequest *,
                    const ACameraMetadata *) {}
void cap_completed(void *, ACameraCaptureSession *, ACaptureRequest *,
                   const ACameraMetadata *) {}

// Pick the front-facing camera id. Returns empty when there isn't one.
std::string front_camera_id(ACameraManager *mgr) {
    ACameraIdList *ids = nullptr;
    if (ACameraManager_getCameraIdList(mgr, &ids) != ACAMERA_OK || !ids) return {};
    std::string found;
    for (int i = 0; i < ids->numCameras; ++i) {
        ACameraMetadata *meta = nullptr;
        if (ACameraManager_getCameraCharacteristics(mgr, ids->cameraIds[i], &meta)
            != ACAMERA_OK)
            continue;
        ACameraMetadata_const_entry e{};
        if (ACameraMetadata_getConstEntry(meta, ACAMERA_LENS_FACING, &e) == ACAMERA_OK
            && e.count > 0 && e.data.u8[0] == ACAMERA_LENS_FACING_FRONT)
            found = ids->cameraIds[i];
        ACameraMetadata_free(meta);
        if (!found.empty()) break;
    }
    ACameraManager_deleteCameraIdList(ids);
    return found;
}

// Largest advertised JPEG size, capped so a 48 MP sensor doesn't hand back a
// 20 MB file we then have to thumbnail anyway.
void best_jpeg_size(ACameraManager *mgr, const std::string &id, int *w, int *h) {
    *w = 1280; *h = 960; // safe default if the query fails
    ACameraMetadata *meta = nullptr;
    if (ACameraManager_getCameraCharacteristics(mgr, id.c_str(), &meta) != ACAMERA_OK)
        return;
    ACameraMetadata_const_entry e{};
    if (ACameraMetadata_getConstEntry(
            meta, ACAMERA_SCALER_AVAILABLE_STREAM_CONFIGURATIONS, &e) == ACAMERA_OK) {
        long bestPx = 0;
        for (uint32_t i = 0; i + 3 < e.count; i += 4) {
            if (e.data.i32[i] != AIMAGE_FORMAT_JPEG) continue;
            if (e.data.i32[i + 3] != ACAMERA_SCALER_AVAILABLE_STREAM_CONFIGURATIONS_OUTPUT)
                continue;
            const int cw = e.data.i32[i + 1], ch = e.data.i32[i + 2];
            const long px = (long)cw * ch;
            if (cw > 3000 || ch > 3000) continue; // cap: plenty for a ride selfie
            if (px > bestPx) { bestPx = px; *w = cw; *h = ch; }
        }
    }
    ACameraMetadata_free(meta);
}

} // namespace

bool available() {
    static int cached = -1;
    if (cached >= 0) return cached == 1;
    ACameraManager *mgr = ACameraManager_create();
    cached = 0;
    if (mgr) {
        cached = front_camera_id(mgr).empty() ? 0 : 1;
        ACameraManager_delete(mgr);
    }
    CAM_LOG("front camera available: %s", cached ? "yes" : "no");
    return cached == 1;
}

bool permission_granted() {
    JNIEnv *e = env();
    jobject activity = cyclomp_activity();
    if (!e || !activity) return false;
    e->PushLocalFrame(4);
    bool granted = false;
    jclass ctx = e->FindClass("android/content/Context");
    jmethodID check = ctx ? e->GetMethodID(ctx, "checkSelfPermission",
                                           "(Ljava/lang/String;)I")
                          : nullptr;
    if (check) {
        jint r = e->CallIntMethod(activity, check, e->NewStringUTF(kPermission));
        granted = !cleared(e) && r == 0; // PackageManager.PERMISSION_GRANTED
    }
    cleared(e);
    e->PopLocalFrame(nullptr);
    return granted;
}

void request_permission() {
    if (g_requested || permission_granted()) return;
    g_requested = true;
    JNIEnv *e = env();
    jobject activity = cyclomp_activity();
    if (!e || !activity) return;
    e->PushLocalFrame(8);
    jclass activityCls = e->FindClass("android/app/Activity");
    jmethodID req = activityCls ? e->GetMethodID(activityCls, "requestPermissions",
                                                 "([Ljava/lang/String;I)V")
                                : nullptr;
    jclass stringCls = e->FindClass("java/lang/String");
    if (req && stringCls) {
        jobjectArray perms = e->NewObjectArray(1, stringCls, nullptr);
        e->SetObjectArrayElement(perms, 0, e->NewStringUTF(kPermission));
        e->CallVoidMethod(activity, req, perms, 2); // 1 is the GPS request code
        // As with location: NativeActivity never forwards the result, so the
        // answer only shows up as a checkSelfPermission flip on a later call.
        if (!cleared(e)) CAM_LOG("requested CAMERA permission");
    }
    cleared(e);
    e->PopLocalFrame(nullptr);
}

void capture(const std::string &path, std::function<void(Shot)> done) {
    Shot shot;
    shot.path = path;

    if (!permission_granted()) {
        request_permission();
        shot.error = "camera permission not granted";
        done(shot);
        return;
    }

    // Everything below is opened and then torn down before returning, in
    // reverse order, including on every failure path.
    ACameraManager *mgr = ACameraManager_create();
    if (!mgr) { shot.error = "no camera manager"; done(shot); return; }

    const std::string id = front_camera_id(mgr);
    if (id.empty()) {
        ACameraManager_delete(mgr);
        shot.error = "no front camera";
        done(shot);
        return;
    }

    int w = 0, h = 0;
    best_jpeg_size(mgr, id, &w, &h);

    Pending pending;
    pending.path = path;

    AImageReader *reader = nullptr;
    ACameraDevice *dev = nullptr;
    ACaptureSessionOutputContainer *outputs = nullptr;
    ACaptureSessionOutput *output = nullptr;
    ACameraOutputTarget *target = nullptr;
    ACaptureRequest *request = nullptr;
    ACameraCaptureSession *session = nullptr;
    ANativeWindow *window = nullptr;

    auto cleanup = [&] {
        if (session) ACameraCaptureSession_close(session);
        if (request) ACaptureRequest_free(request);
        if (target) ACameraOutputTarget_free(target);
        if (outputs && output) ACaptureSessionOutputContainer_remove(outputs, output);
        if (output) ACaptureSessionOutput_free(output);
        if (outputs) ACaptureSessionOutputContainer_free(outputs);
        if (dev) ACameraDevice_close(dev);
        if (window) ANativeWindow_release(window);
        if (reader) AImageReader_delete(reader);
        ACameraManager_delete(mgr);
    };
    auto fail = [&](const char *why) {
        cleanup();
        shot.error = why;
        CAM_ERR("%s", why);
        done(shot);
    };

    if (AImageReader_new(w, h, AIMAGE_FORMAT_JPEG, 2, &reader) != AMEDIA_OK)
        return fail("AImageReader_new failed");
    AImageReader_ImageListener listener{&pending, on_image};
    AImageReader_setImageListener(reader, &listener);
    if (AImageReader_getWindow(reader, &window) != AMEDIA_OK || !window)
        return fail("no reader window");
    ANativeWindow_acquire(window);

    ACameraDevice_StateCallbacks devCb{&pending, dev_disconnected, dev_error};
    if (ACameraManager_openCamera(mgr, id.c_str(), &devCb, &dev) != ACAMERA_OK || !dev)
        return fail("openCamera failed");

    if (ACaptureSessionOutputContainer_create(&outputs) != ACAMERA_OK)
        return fail("output container failed");
    if (ACaptureSessionOutput_create(window, &output) != ACAMERA_OK)
        return fail("session output failed");
    ACaptureSessionOutputContainer_add(outputs, output);

    ACameraCaptureSession_stateCallbacks sesCb{&pending, ses_closed, ses_ready, ses_active};
    if (ACameraDevice_createCaptureSession(dev, outputs, &sesCb, &session) != ACAMERA_OK)
        return fail("createCaptureSession failed");

    if (ACameraDevice_createCaptureRequest(dev, TEMPLATE_STILL_CAPTURE, &request)
        != ACAMERA_OK)
        return fail("createCaptureRequest failed");
    if (ACameraOutputTarget_create(window, &target) != ACAMERA_OK)
        return fail("output target failed");
    ACaptureRequest_addTarget(request, target);

    // Let the HAL do its own 3A; a moving bike gives it no time for anything
    // clever anyway.
    const uint8_t afMode = ACAMERA_CONTROL_AF_MODE_CONTINUOUS_PICTURE;
    ACaptureRequest_setEntry_u8(request, ACAMERA_CONTROL_AF_MODE, 1, &afMode);
    const uint8_t aeMode = ACAMERA_CONTROL_AE_MODE_ON;
    ACaptureRequest_setEntry_u8(request, ACAMERA_CONTROL_AE_MODE, 1, &aeMode);

    ACameraCaptureSession_captureCallbacks capCb{
        &pending, cap_started, cap_progressed, cap_completed, cap_failed, nullptr, nullptr};
    if (ACameraCaptureSession_capture(session, &capCb, 1, &request, nullptr) != ACAMERA_OK)
        return fail("capture submit failed");

    {   // The HAL needs a moment to converge 3A and deliver; bail rather than
        // hang the caller forever if it never does.
        std::unique_lock<std::mutex> lk(pending.m);
        pending.cv.wait_for(lk, std::chrono::milliseconds(4000),
                            [&] { return pending.done; });
    }

    cleanup();

    if (!pending.done) { shot.error = "timed out waiting for frame"; done(shot); return; }
    shot.ok = pending.ok;
    shot.error = pending.error;
    shot.width = pending.width;
    shot.height = pending.height;
    if (shot.ok) CAM_LOG("captured %dx%d -> %s", shot.width, shot.height, path.c_str());
    else CAM_ERR("capture failed: %s", shot.error.c_str());
    done(shot);
}

} // namespace selfie
#endif // __ANDROID__
