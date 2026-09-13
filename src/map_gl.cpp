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
#include <mapbox/geojson.hpp>
#include <mln/style/layers/line_layer.hpp>
#include <mln/style/sources/geojson_source.hpp>
#include <mln/style/style.hpp>
#include <mln/util/run_loop.hpp>

#include <android/hardware_buffer.h>
#include <android/log.h>
#include <EGL/egl.h>
#include <EGL/eglext.h>
#include <GLES3/gl3.h>
#include <GLES2/gl2ext.h>

#include <algorithm>
#include <atomic>
#include <cmath>
#include <condition_variable>
#include <exception>
#include <mutex>
#include <chrono>
#include <thread>

#define MAPGL_LOG(...) __android_log_print(ANDROID_LOG_INFO, "cyclomp-mapgl", __VA_ARGS__)

namespace mapgl {
namespace {

constexpr const char *kStyleUrl = "https://tiles.openfreemap.org/styles/dark";
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

    std::function<void(uint32_t, uint32_t, uint32_t, uint64_t)> sink;

    std::mutex m;
    std::condition_variable cv;
    // pending camera commands (coalesced; applied on the map thread)
    uint64_t cmdGen = 0; // bumped per accepted command; frames report theirs
    double lat = -6.9147, lon = 107.6098, zoom = 15; // Bandung
    bool centerDirty = true;
    bool renderDirty = false;      // re-render without camera change (poke)
    double dx = 0, dy = 0;         // accumulated pan (logical px)
    double scale = 1.0;            // accumulated pinch factor
    double bearing = 0;            // heading-up camera rotation
    bool bearingDirty = false;
    double pitch = 0;              // 2.5D chase tilt (degrees; 0 = top-down)
    bool pitchDirty = false;
    double ax = 0, ay = 0;         // pinch anchor (logical px)

    // Live ride track (lat/lon pairs, UI thread writes, map thread applies).
    std::vector<std::pair<double, double>> track;
    bool trackDirty = false;

    // Rider marker: set from the UI thread, projected on the map thread.
    double markerLat = 0, markerLon = 0;
    bool markerSet = false;
    std::atomic<bool> markerProjected{false};
    std::atomic<double> markerNx{0}, markerNy{0}, markerAspect{1};

    // Static start/end bullets on the detail map (projected each render so
    // they track pan/zoom; shown when a ride replay is NOT running).
    double epSLat = 0, epSLon = 0, epELat = 0, epELon = 0;
    bool epSet = false;
    std::atomic<bool> epProjected{false};
    std::atomic<double> epSNx{0}, epSNy{0}, epENx{0}, epENy{0};

    AHardwareBuffer *ahb[kBufs] = {nullptr, nullptr, nullptr};
    EGLImageKHR image[kBufs] = {EGL_NO_IMAGE_KHR, EGL_NO_IMAGE_KHR,
                                EGL_NO_IMAGE_KHR};
    std::atomic<bool> buffersReady{false};

    // UI-context textures (created in the rendering notifier)
    GLuint uiTex[kBufs] = {0, 0, 0};
    std::atomic<bool> uiImported{false};

    std::atomic<bool> started{false};

    // Road line brightness, 0..100 (100 = the tuned reference colours below).
    std::atomic<int> roadBrightness{100};
    std::atomic<bool> roadDirty{true};
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

    // The stock dark style keeps roads at 7-16% lightness — near invisible
    // on the phone. Brighten the road line layers (with a slight phosphor
    // tint) once the style has loaded; getLayer returns null until then,
    // so this retries each loop pass until it lands.
    bool roadsBrightened = false;
    auto brighten_roads = [&map, &roadsBrightened, &s] {
        // Re-apply when the brightness changed, or keep retrying until the
        // style has loaded (getLayer returns null before then).
        bool dirty = s.roadDirty.exchange(false);
        if (roadsBrightened && !dirty) return;
        // The tuned reference colours sit at 50; the slider scales RGB from 0
        // (roads off) through 1x (=50) up to 2x at 100 (clamped to white),
        // alpha untouched.
        double k = std::clamp(s.roadBrightness.load(), 0, 100) / 50.0;
        const std::pair<const char *, mln::Color> recolor[] = {
            {"highway_minor", {0.20f, 0.22f, 0.20f, 1.f}},
            {"highway_path", {0.16f, 0.18f, 0.16f, 1.f}},
            {"highway_major_inner", {0.29f, 0.31f, 0.29f, 1.f}},
            {"highway_major_subtle", {0.25f, 0.27f, 0.25f, 1.f}},
            {"highway_major_casing", {0.33f, 0.33f, 0.33f, 0.8f}},
            {"highway_motorway_inner", {0.33f, 0.35f, 0.33f, 1.f}},
            {"highway_motorway_subtle", {0.22f, 0.24f, 0.22f, 1.f}},
            {"highway_motorway_casing", {0.36f, 0.36f, 0.36f, 0.8f}},
        };
        bool any = false;
        for (const auto &r : recolor) {
            if (auto *l = static_cast<mln::style::LineLayer *>(
                    map.getStyle().getLayer(r.first))) {
                const mln::Color &c = r.second;
                l->setLineColor(mln::Color{(float)std::min(c.r * k, 1.0),
                                           (float)std::min(c.g * k, 1.0),
                                           (float)std::min(c.b * k, 1.0), c.a});
                any = true;
            }
        }
        if (any) {
            roadsBrightened = true;
            MAPGL_LOG("roads recoloured (brightness %d)", s.roadBrightness.load());
        }
    };

    // Camera tween state (map-thread only). The public set_bearing/set_pitch
    // push a TARGET; the loop eases these CURRENT values toward it each frame
    // so rotation and tilt glide instead of snapping. While a tween is in
    // flight the loop stops blocking and paces itself at ~60fps.
    double bearingCur = 0.0, pitchCur = 0.0;
    bool animating = false;
    constexpr double kTweenEase = 0.30; // per-frame approach fraction

    for (;;) {
        bool ctr, mset, trk, brgDirty, pchDirty, epSet;
        double la, lo, zm, dx, dy, sc, ax, ay, mlat, mlon, brg, pch;
        double epSLat, epSLon, epELat, epELon;
        std::vector<std::pair<double, double>> trackPts;
        uint64_t gen;
        {
            std::unique_lock<std::mutex> lk(s.m);
            auto ready = [&] {
                return s.centerDirty || s.renderDirty || s.dx != 0 ||
                       s.dy != 0 || s.scale != 1.0 || s.trackDirty ||
                       s.bearingDirty || s.pitchDirty;
            };
            // Mid-tween: wake on the frame clock (or sooner on a command);
            // otherwise sleep until the next command.
            if (animating)
                s.cv.wait_for(lk, std::chrono::milliseconds(16), ready);
            else
                s.cv.wait(lk, ready);
            gen = s.cmdGen;
            trk = s.trackDirty;
            s.trackDirty = false;
            if (trk) trackPts = s.track;
            brgDirty = s.bearingDirty;
            s.bearingDirty = false;
            brg = s.bearing;
            pchDirty = s.pitchDirty;
            s.pitchDirty = false;
            pch = s.pitch;
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
            epSet = s.epSet;
            epSLat = s.epSLat;
            epSLon = s.epSLon;
            epELat = s.epELat;
            epELon = s.epELon;
        }
        brighten_roads();
        // Ease bearing (shortest angular path) and pitch toward their targets.
        double bd = brg - bearingCur;
        while (bd > 180.0) bd -= 360.0;
        while (bd < -180.0) bd += 360.0;
        double pd = pch - pitchCur;
        bool brgAnim = std::fabs(bd) > 0.05;
        bool pchAnim = std::fabs(pd) > 0.05;
        bearingCur = brgAnim ? bearingCur + bd * kTweenEase : brg;
        pitchCur = pchAnim ? pitchCur + pd * kTweenEase : pch;
        if (bearingCur < 0.0) bearingCur += 360.0;
        else if (bearingCur >= 360.0) bearingCur -= 360.0;
        animating = brgAnim || pchAnim;
        if (ctr || brgDirty || pchDirty || animating) {
            mln::CameraOptions cam;
            if (ctr) {
                cam.withCenter(mln::LatLng{la, lo});
                if (zm > 0) cam.withZoom(zm);
            }
            cam.withBearing(bearingCur);
            cam.withPitch(pitchCur);
            map.jumpTo(cam);
        }
        if (sc != 1.0)
            map.scaleBy(sc, mln::ScreenCoordinate{ax, ay});
        if (dx != 0 || dy != 0) map.moveBy(mln::ScreenCoordinate{dx, dy});

        // Live route line (Bauhaus yellow). Source/layer added lazily on the
        // first update; the style is loaded by then (the map has rendered).
        if (trk) {
            static bool trackLayerAdded = false;
            try {
                if (!trackLayerAdded) {
                    map.getStyle().addSource(
                        std::make_unique<mln::style::GeoJSONSource>("cyclomp-track"));
                    auto line = std::make_unique<mln::style::LineLayer>(
                        "cyclomp-track-line", "cyclomp-track");
                    line->setLineColor(mln::Color{0.953f, 0.772f, 0.0f, 1.f});
                    line->setLineWidth(4.f);
                    line->setLineCap(mln::style::LineCapType::Round);
                    line->setLineJoin(mln::style::LineJoinType::Round);
                    map.getStyle().addLayer(std::move(line));
                    trackLayerAdded = true;
                }
                mapbox::geometry::line_string<double> ls;
                ls.reserve(trackPts.size());
                for (auto &p : trackPts) ls.push_back({p.second, p.first}); // x=lon
                auto *src = static_cast<mln::style::GeoJSONSource *>(
                    map.getStyle().getSource("cyclomp-track"));
                if (src)
                    src->setGeoJSON(mapbox::geojson::geojson{
                        mapbox::geometry::geometry<double>{ls}});
            } catch (const std::exception &e) {
                MAPGL_LOG("track layer error: %s", e.what());
            }
        }

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
        if (epSet) {
            auto ps = map.pixelForLatLng(mln::LatLng{epSLat, epSLon});
            auto pe = map.pixelForLatLng(mln::LatLng{epELat, epELon});
            s.epSNx.store((ps.x - lw / 2.0) / lw);
            s.epSNy.store((ps.y - lh / 2.0) / lh);
            s.epENx.store((pe.x - lw / 2.0) / lw);
            s.epENy.store((pe.y - lh / 2.0) / lh);
            s.markerAspect.store((double)lw / (double)lh);
            s.epProjected.store(true);
        }

        int idx = cur;
        cur = (cur + 1) % kBufs;
        if (s.sink) s.sink((uint32_t)idx, physSize.width, physSize.height, gen);
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

void set_frame_sink(
    std::function<void(uint32_t, uint32_t, uint32_t, uint64_t)> sink) {
    svc().sink = std::move(sink);
}

bool ui_import_all() {
    auto &s = svc();
    if (s.uiImported.load()) {
        // An app switch can silently recreate Slint's GL context; the old
        // texture ids then belong to a dead context and the map turns
        // blank. Detect that and re-import into the new context.
        if (glIsTexture(s.uiTex[0])) return true;
        MAPGL_LOG("UI textures stale (context recreated) — re-importing");
        ui_reset();
    }
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
    // Push a fresh frame through the new textures — the Image the UI still
    // holds references a texture from the old context.
    poke();
    return true;
}

void ui_reset() {
    auto &s = svc();
    s.uiImported.store(false);
    // No GL calls here: the context these ids lived in is already gone.
    for (int i = 0; i < kBufs; ++i) s.uiTex[i] = 0;
    MAPGL_LOG("UI textures reset (context teardown)");
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
        ++s.cmdGen;
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

void set_track(std::vector<std::pair<double, double>> pts) {
    auto &s = svc();
    {
        std::lock_guard<std::mutex> lk(s.m);
        s.track = std::move(pts);
        s.trackDirty = true;
        s.renderDirty = true; // repaint even when the camera is idle
    }
    s.cv.notify_one();
}

bool marker_offset(double &nx, double &ny, double &aspect) {
    auto &s = svc();
    if (!s.markerProjected.load()) return false;
    nx = s.markerNx.load();
    ny = s.markerNy.load();
    aspect = s.markerAspect.load();
    return true;
}

void set_endpoints(double slat, double slon, double elat, double elon) {
    auto &s = svc();
    {
        std::lock_guard<std::mutex> lk(s.m);
        s.epSLat = slat;
        s.epSLon = slon;
        s.epELat = elat;
        s.epELon = elon;
        s.epSet = true;
        s.renderDirty = true; // re-render so the projection follows
    }
    s.cv.notify_one();
}

void clear_endpoints() {
    auto &s = svc();
    std::lock_guard<std::mutex> lk(s.m);
    s.epSet = false;
    s.epProjected.store(false);
}

bool endpoints_offset(double &snx, double &sny, double &enx, double &eny,
                      double &aspect) {
    auto &s = svc();
    if (!s.epProjected.load()) return false;
    snx = s.epSNx.load();
    sny = s.epSNy.load();
    enx = s.epENx.load();
    eny = s.epENy.load();
    aspect = s.markerAspect.load();
    return true;
}

void drag_by(double dx, double dy) {
    auto &s = svc();
    {
        std::lock_guard<std::mutex> lk(s.m);
        s.dx += dx;
        s.dy += dy;
        ++s.cmdGen;
    }
    s.cv.notify_one();
}

uint64_t scale_by(double factor, double ax, double ay) {
    auto &s = svc();
    uint64_t gen;
    {
        std::lock_guard<std::mutex> lk(s.m);
        if (factor > 0) {
            s.scale *= factor;
            s.ax = ax;
            s.ay = ay;
            ++s.cmdGen;
        }
        gen = s.cmdGen;
    }
    s.cv.notify_one();
    return gen;
}

void zoom_step(int delta) {
    auto &s = svc();
    // anchor = map centre in logical px
    double ax = (s.widthPx / s.ratio) / 2.0;
    double ay = (s.heightPx / s.ratio) / 2.0;
    scale_by(delta > 0 ? 2.0 : 0.5, ax, ay);
}

void set_bearing(double deg) {
    MAPGL_LOG("bearing -> %.0f", deg);
    auto &s = svc();
    {
        std::lock_guard<std::mutex> lk(s.m);
        s.bearing = deg;
        s.bearingDirty = true;
        ++s.cmdGen;
    }
    s.cv.notify_one();
}

void set_pitch(double deg) {
    MAPGL_LOG("pitch -> %.0f", deg);
    auto &s = svc();
    {
        std::lock_guard<std::mutex> lk(s.m);
        s.pitch = deg;
        s.pitchDirty = true;
        ++s.cmdGen;
    }
    s.cv.notify_one();
}

void set_road_brightness(int pct) {
    auto &s = svc();
    if (pct < 0) pct = 0;
    else if (pct > 100) pct = 100;
    s.roadBrightness.store(pct);
    s.roadDirty.store(true);
    {
        std::lock_guard<std::mutex> lk(s.m);
        s.renderDirty = true; // re-apply the recolour and repaint
    }
    s.cv.notify_one();
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
