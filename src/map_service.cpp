#include "map_service.h"

#include <mln/gfx/headless_frontend.hpp>
#include <mln/map/map.hpp>
#include <mln/map/map_observer.hpp>
#include <mln/map/map_options.hpp>
#include <mln/storage/resource_options.hpp>
#include <mln/style/style.hpp>
#include <mln/util/run_loop.hpp>

#include <condition_variable>
#include <mutex>
#include <thread>

// Per-frame tracing, off unless -DCYCLOMP_MAP_VERBOSE=ON.
#if defined(CYCLOMP_MAP_VERBOSE)
#ifdef __ANDROID__
#include <android/log.h>
#define MAP_LOG(...) __android_log_print(ANDROID_LOG_INFO, "cyclomp-map", __VA_ARGS__)
#else
#include <cstdio>
#define MAP_LOG(...) std::fprintf(stderr, __VA_ARGS__)
#endif
#else
#define MAP_LOG(...) ((void)0)
#endif

namespace {
constexpr const char *kStyleUrl = "https://tiles.openfreemap.org/styles/positron";
#ifdef __ANDROID__
// App-private cache dir; created by the system at install time.
constexpr const char *kCachePath =
    "/data/data/dev.bauhouse.cyclomp/cache/cyclomp-mbgl.sqlite";
#else
constexpr const char *kCachePath = "/tmp/cyclomp-mbgl-cache.sqlite";
#endif
} // namespace

struct MapService::Impl {
    uint32_t width;
    uint32_t height;
    FrameCallback on_frame;

    std::mutex m;
    std::condition_variable cv;
    bool dirty = false;
    bool quit = false;
    double lat = 0, lon = 0, zoom = 15;

    std::thread worker;

    void run() {
        MAP_LOG("map thread start");
        // The run loop, frontend and map must all live on this thread.
        mln::util::RunLoop loop;
        MAP_LOG("runloop up");
        mln::HeadlessFrontend frontend({width, height}, 1.0f);
        MAP_LOG("frontend up");
        mln::Map map(frontend, mln::MapObserver::nullObserver(),
                     mln::MapOptions()
                         .withMapMode(mln::MapMode::Static)
                         .withSize(frontend.getSize())
                         .withPixelRatio(1.0f),
                     mln::ResourceOptions()
                         .withCachePath(kCachePath)
                         .withAssetPath("."));
        map.getStyle().loadURL(kStyleUrl);
        MAP_LOG("style url set");

        for (;;) {
            double la, lo, zm;
            {
                std::unique_lock<std::mutex> lk(m);
                cv.wait(lk, [&] { return dirty || quit; });
                if (quit) return;
                la = lat;
                lo = lon;
                zm = zoom;
                dirty = false;
            }
            map.jumpTo(mln::CameraOptions()
                           .withCenter(mln::LatLng{la, lo})
                           .withZoom(zm));
            try {
                MAP_LOG("render start %f,%f z%f", la, lo, zm);
                auto image = frontend.render(map).image;
                MAP_LOG("render done");
                std::vector<uint8_t> rgba(image.data.get(),
                                          image.data.get() + image.bytes());
                on_frame(image.size.width, image.size.height, std::move(rgba));
            } catch (const std::exception &) {
                // Tile/network hiccup — keep the previous frame on screen.
            }
        }
    }
};

MapService::MapService(uint32_t width, uint32_t height, FrameCallback on_frame)
    : impl(std::make_unique<Impl>()) {
    impl->width = width;
    impl->height = height;
    impl->on_frame = std::move(on_frame);
    impl->worker = std::thread([this] { impl->run(); });
}

MapService::~MapService() {
    {
        std::lock_guard<std::mutex> lk(impl->m);
        impl->quit = true;
    }
    impl->cv.notify_one();
    impl->worker.join();
}

void MapService::set_camera(double lat, double lon, double zoom) {
    {
        std::lock_guard<std::mutex> lk(impl->m);
        if (impl->dirty == false && impl->lat == lat && impl->lon == lon &&
            impl->zoom == zoom)
            return;
        impl->lat = lat;
        impl->lon = lon;
        impl->zoom = zoom;
        impl->dirty = true;
    }
    impl->cv.notify_one();
}
