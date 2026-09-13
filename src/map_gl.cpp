// B-lite implementation — see map_gl.h.
#if defined(__ANDROID__) && defined(CYCLOMP_MAP_GL)
#include "map_gl.h"

#include <mln/gfx/backend_scope.hpp>
#include <mln/gfx/headless_frontend.hpp>
#include <mln/gfx/renderable.hpp>
#include <mln/gl/renderable_resource.hpp>
#include <mln/map/map.hpp>
#include <mln/map/map_observer.hpp>
#include <mln/map/map_options.hpp>
#include <mln/storage/resource_options.hpp>
#include <mln/style/style.hpp>
#include <mln/util/run_loop.hpp>

#include <android/hardware_buffer.h>
#include <android/log.h>
#include <EGL/egl.h>
#include <EGL/eglext.h>
#include <GLES3/gl3.h>
#include <GLES2/gl2ext.h>

#include <atomic>
#include <condition_variable>
#include <exception>
#include <mutex>
#include <thread>

#define MAPGL_LOG(...) __android_log_print(ANDROID_LOG_INFO, "cyclomp-mapgl", __VA_ARGS__)

namespace mapgl {
namespace {

constexpr const char *kStyleUrl = "https://tiles.openfreemap.org/styles/liberty";
constexpr const char *kCachePath =
    "/data/data/dev.bauhouse.cyclomp/cache/cyclomp-mbgl.sqlite";
constexpr int kBufs = 3;

// EGL/GL extension entry points (resolved once; usable from any GL thread).
PFNEGLGETNATIVECLIENTBUFFERANDROIDPROC p_eglGetNativeClientBufferANDROID;
PFNEGLCREATEIMAGEKHRPROC p_eglCreateImageKHR;
PFNGLEGLIMAGETARGETTEXTURE2DOESPROC p_glEGLImageTargetTexture2DOES;

bool resolve_ext() {
    if (p_glEGLImageTargetTexture2DOES) return true;
    p_eglGetNativeClientBufferANDROID =
        (PFNEGLGETNATIVECLIENTBUFFERANDROIDPROC)eglGetProcAddress(
            "eglGetNativeClientBufferANDROID");
    p_eglCreateImageKHR =
        (PFNEGLCREATEIMAGEKHRPROC)eglGetProcAddress("eglCreateImageKHR");
    p_glEGLImageTargetTexture2DOES =
        (PFNGLEGLIMAGETARGETTEXTURE2DOESPROC)eglGetProcAddress(
            "glEGLImageTargetTexture2DOES");
    return p_eglGetNativeClientBufferANDROID && p_eglCreateImageKHR &&
           p_glEGLImageTargetTexture2DOES;
}

struct Service {
    uint32_t widthPx = 0, heightPx = 0; // physical buffer size
    float ratio = 1.f;

    std::function<void(uint32_t, uint32_t, uint32_t)> sink;

    std::mutex m;
    std::condition_variable cv;
    // pending camera commands (coalesced; applied on the map thread)
    double lat = -6.9147, lon = 107.6098, zoom = 15; // Bandung
    bool centerDirty = true;
    bool renderDirty = false;      // re-render without camera change (poke)
    double dx = 0, dy = 0;         // accumulated pan (logical px)
    double scale = 1.0;            // accumulated pinch factor
    double ax = 0, ay = 0;         // pinch anchor (logical px)

    // Rider marker: set from the UI thread, projected on the map thread.
    double markerLat = 0, markerLon = 0;
    bool markerSet = false;
    std::atomic<bool> markerProjected{false};
    std::atomic<double> markerNx{0}, markerNy{0}, markerAspect{1};

    AHardwareBuffer *ahb[kBufs] = {nullptr, nullptr, nullptr};
    EGLImageKHR image[kBufs] = {EGL_NO_IMAGE_KHR, EGL_NO_IMAGE_KHR,
                                EGL_NO_IMAGE_KHR};
    std::atomic<bool> buffersReady{false};

    // UI-context textures (created in the rendering notifier)
    GLuint uiTex[kBufs] = {0, 0, 0};
    std::atomic<bool> uiImported{false};

    std::atomic<bool> started{false};
};

Service &svc() {
    static Service s;
    return s;
}

bool alloc_buffers(uint32_t w, uint32_t h) {
    auto &s = svc();
    if (!resolve_ext()) {
        MAPGL_LOG("EGLImage/AHB extensions missing");
        return false;
    }
    EGLDisplay dpy = eglGetDisplay(EGL_DEFAULT_DISPLAY);
    AHardwareBuffer_Desc desc = {};
    desc.width = w;
    desc.height = h;
    desc.layers = 1;
    desc.format = AHARDWAREBUFFER_FORMAT_R8G8B8A8_UNORM;
    desc.usage = AHARDWAREBUFFER_USAGE_GPU_SAMPLED_IMAGE |
                 AHARDWAREBUFFER_USAGE_GPU_COLOR_OUTPUT;
    for (int i = 0; i < kBufs; ++i) {
        if (AHardwareBuffer_allocate(&desc, &s.ahb[i]) != 0) {
            MAPGL_LOG("AHardwareBuffer_allocate FAILED (buf %d)", i);
            return false;
        }
        EGLClientBuffer cb = p_eglGetNativeClientBufferANDROID(s.ahb[i]);
        const EGLint attrs[] = {EGL_NONE};
        s.image[i] = p_eglCreateImageKHR(dpy, EGL_NO_CONTEXT,
                                         EGL_NATIVE_BUFFER_ANDROID, cb, attrs);
        if (s.image[i] == EGL_NO_IMAGE_KHR) {
            MAPGL_LOG("eglCreateImageKHR FAILED (buf %d, err=0x%x)", i,
                      eglGetError());
            return false;
        }
    }
    MAPGL_LOG("3x AHardwareBuffer %ux%u ready", w, h);
    s.buffersReady.store(true);
    return true;
}

void threadMain() {
    auto &s = svc();
    MAPGL_LOG("map thread start (B-lite) %ux%u ratio=%.2f", s.widthPx,
              s.heightPx, s.ratio);

    mln::util::RunLoop loop;

    // Some mobile GPUs refuse the full-screen headless framebuffer (Mali-G52
    // on the Redmi 9 throws "Couldn't create framebuffer" at 1078x2338).
    // Fall back through smaller render sizes; the Image upscales with cover.
    const float scales[] = {1.0f, 0.75f, 0.5f};
    std::unique_ptr<mln::HeadlessFrontend> frontendPtr;
    uint32_t lw = 0, lh = 0;
    float ratio = s.ratio;
    for (float sc : scales) {
        lw = (uint32_t)(s.widthPx * sc / s.ratio);
        lh = (uint32_t)(s.heightPx * sc / s.ratio);
        try {
            frontendPtr = std::make_unique<mln::HeadlessFrontend>(
                mln::Size{lw, lh}, ratio);
            MAPGL_LOG("headless frontend %ux%u @%.2f (scale %.2f)", lw, lh,
                      ratio, sc);
            break;
        } catch (const std::exception &e) {
            MAPGL_LOG("headless %ux%u failed: %s", lw, lh, e.what());
            frontendPtr.reset();
        }
    }
    if (!frontendPtr) {
        MAPGL_LOG("no workable headless size — map disabled");
        return;
    }
    auto &frontend = *frontendPtr;
    mln::Map map(frontend, mln::MapObserver::nullObserver(),
                 mln::MapOptions()
                     .withMapMode(mln::MapMode::Static)
                     .withSize({lw, lh})
                     .withPixelRatio(ratio),
                 mln::ResourceOptions()
                     .withCachePath(kCachePath)
                     .withAssetPath("."));
    map.getStyle().loadURL(kStyleUrl);

    // Physical size the headless backend renders at (logical * ratio).
    // NOTE: don't touch getDefaultRenderable() outside a BackendScope — it
    // lazily creates the framebuffer and throws without a current context.
    const mln::Size physSize{(uint32_t)(lw * ratio), (uint32_t)(lh * ratio)};
    if (!alloc_buffers(physSize.width, physSize.height)) return;

    // Map-side textures bound to the shared buffers (created lazily inside
    // the first BackendScope, when the headless GL context exists).
    GLuint mapTex[kBufs] = {0, 0, 0};
    bool mapTexReady = false;
    int cur = 0;

    for (;;) {
        bool ctr, mset;
        double la, lo, zm, dx, dy, sc, ax, ay, mlat, mlon;
        {
            std::unique_lock<std::mutex> lk(s.m);
            s.cv.wait(lk, [&] {
                return s.centerDirty || s.renderDirty || s.dx != 0 ||
                       s.dy != 0 || s.scale != 1.0;
            });
            ctr = s.centerDirty;
            s.centerDirty = false;
            s.renderDirty = false;
            la = s.lat;
            lo = s.lon;
            zm = s.zoom;
            dx = s.dx;
            dy = s.dy;
            s.dx = s.dy = 0;
            sc = s.scale;
            ax = s.ax;
            ay = s.ay;
            s.scale = 1.0;
            mset = s.markerSet;
            mlat = s.markerLat;
            mlon = s.markerLon;
        }
        if (ctr) {
            auto cam = mln::CameraOptions().withCenter(mln::LatLng{la, lo});
            if (zm > 0) cam.withZoom(zm);
            map.jumpTo(cam);
        }
        if (sc != 1.0)
            map.scaleBy(sc, mln::ScreenCoordinate{ax, ay});
        if (dx != 0 || dy != 0) map.moveBy(mln::ScreenCoordinate{dx, dy});

        // Same flow as HeadlessFrontend::render(), with a GPU blit instead of
        // readStillImage(): renderStill waits until all tiles are loaded.
        std::exception_ptr error;
        bool done = false;
        {
            mln::gfx::BackendScope guard{*frontend.getBackend()};
            map.renderStill([&](const std::exception_ptr &e) {
                error = e;
                done = true;
            });
            while (!done) mln::util::RunLoop::Get()->runOnce();
            if (error) {
                try {
                    std::rethrow_exception(error);
                } catch (const std::exception &e) {
                    MAPGL_LOG("renderStill error: %s", e.what());
                }
                continue;
            }
            if (!mapTexReady) {
                glGenTextures(kBufs, mapTex);
                for (int i = 0; i < kBufs; ++i) {
                    glBindTexture(GL_TEXTURE_2D, mapTex[i]);
                    p_glEGLImageTargetTexture2DOES(GL_TEXTURE_2D,
                                                   (GLeglImageOES)s.image[i]);
                }
                mapTexReady = true;
                MAPGL_LOG("map-side AHB textures imported");
            }
            // Read from the headless framebuffer into the shared buffer.
            frontend.getBackend()
                ->getDefaultRenderable()
                .getResource<mln::gl::RenderableResource>()
                .bind();
            glBindTexture(GL_TEXTURE_2D, mapTex[cur]);
            glCopyTexSubImage2D(GL_TEXTURE_2D, 0, 0, 0, 0, 0,
                                (GLsizei)physSize.width,
                                (GLsizei)physSize.height);
            glFinish(); // buffer must be complete before the UI samples it
        }
        // Project the rider onto the frame we just rendered. Normalised so
        // the UI can place it without knowing the texture size.
        if (mset) {
            auto p = map.pixelForLatLng(mln::LatLng{mlat, mlon});
            s.markerNx.store((p.x - lw / 2.0) / lw);
            s.markerNy.store((p.y - lh / 2.0) / lh);
            s.markerAspect.store((double)lw / (double)lh);
            s.markerProjected.store(true);
        }

        int idx = cur;
        cur = (cur + 1) % kBufs;
        if (s.sink) s.sink((uint32_t)idx, physSize.width, physSize.height);
    }
}

} // namespace

void setup(uint32_t width_px, uint32_t height_px, float pixel_ratio) {
    auto &s = svc();
    if (s.started.exchange(true)) return;
    s.widthPx = width_px;
    s.heightPx = height_px;
    s.ratio = pixel_ratio;
    std::thread(threadMain).detach();
}

void set_frame_sink(std::function<void(uint32_t, uint32_t, uint32_t)> sink) {
    svc().sink = std::move(sink);
}

bool ui_import_all() {
    auto &s = svc();
    if (s.uiImported.load()) return true;
    if (!s.buffersReady.load()) return false;
    if (!resolve_ext()) return false;
    glGenTextures(kBufs, s.uiTex);
    for (int i = 0; i < kBufs; ++i) {
        glBindTexture(GL_TEXTURE_2D, s.uiTex[i]);
        p_glEGLImageTargetTexture2DOES(GL_TEXTURE_2D, (GLeglImageOES)s.image[i]);
        glTexParameteri(GL_TEXTURE_2D, GL_TEXTURE_MIN_FILTER, GL_LINEAR);
        glTexParameteri(GL_TEXTURE_2D, GL_TEXTURE_MAG_FILTER, GL_LINEAR);
        glTexParameteri(GL_TEXTURE_2D, GL_TEXTURE_WRAP_S, GL_CLAMP_TO_EDGE);
        glTexParameteri(GL_TEXTURE_2D, GL_TEXTURE_WRAP_T, GL_CLAMP_TO_EDGE);
    }
    s.uiImported.store(true);
    MAPGL_LOG("UI-side AHB textures imported");
    return true;
}

uint32_t ui_texture(uint32_t idx) {
    return idx < kBufs ? svc().uiTex[idx] : 0;
}

void set_camera(double lat, double lon, double zoom) {
    auto &s = svc();
    {
        std::lock_guard<std::mutex> lk(s.m);
        s.lat = lat;
        s.lon = lon;
        s.zoom = zoom;
        s.centerDirty = true;
    }
    s.cv.notify_one();
}

void set_marker(double lat, double lon) {
    auto &s = svc();
    bool moved = false;
    {
        std::lock_guard<std::mutex> lk(s.m);
        if (!s.markerSet || s.markerLat != lat || s.markerLon != lon) {
            s.markerLat = lat;
            s.markerLon = lon;
            s.markerSet = true;
            s.renderDirty = true; // re-render so the projection follows
            moved = true;
        }
    }
    if (moved) s.cv.notify_one();
}

bool marker_offset(double &nx, double &ny, double &aspect) {
    auto &s = svc();
    if (!s.markerProjected.load()) return false;
    nx = s.markerNx.load();
    ny = s.markerNy.load();
    aspect = s.markerAspect.load();
    return true;
}

void drag_by(double dx, double dy) {
    auto &s = svc();
    {
        std::lock_guard<std::mutex> lk(s.m);
        s.dx += dx;
        s.dy += dy;
    }
    s.cv.notify_one();
}

void scale_by(double factor, double ax, double ay) {
    if (factor <= 0) return;
    auto &s = svc();
    {
        std::lock_guard<std::mutex> lk(s.m);
        s.scale *= factor;
        s.ax = ax;
        s.ay = ay;
    }
    s.cv.notify_one();
}

void zoom_step(int delta) {
    auto &s = svc();
    // anchor = map centre in logical px
    double ax = (s.widthPx / s.ratio) / 2.0;
    double ay = (s.heightPx / s.ratio) / 2.0;
    scale_by(delta > 0 ? 2.0 : 0.5, ax, ay);
}

void poke() {
    auto &s = svc();
    {
        std::lock_guard<std::mutex> lk(s.m);
        s.renderDirty = true;
    }
    s.cv.notify_one();
}

} // namespace mapgl
#endif // __ANDROID__ && CYCLOMP_MAP_GL
