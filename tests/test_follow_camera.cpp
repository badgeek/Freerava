#include <doctest/doctest.h>

#include "core/follow_camera.h"

using namespace core;

TEST_CASE("first position always fires with the initial zoom") {
    FollowCamera cam(15.0);
    CHECK_FALSE(cam.placed());
    cam.release(); // even released, the first fix must place the camera
    auto up = cam.on_position(-6.9, 107.6);
    REQUIRE(up.has_value());
    CHECK(up->zoom == doctest::Approx(15.0));
    CHECK(cam.placed());
}

TEST_CASE("subsequent updates keep the current zoom") {
    FollowCamera cam;
    cam.on_position(-6.9, 107.6);
    auto up = cam.on_position(-6.91, 107.61);
    REQUIRE(up.has_value());
    CHECK(up->zoom < 0);
}

TEST_CASE("release stops updates, relock resumes them") {
    FollowCamera cam;
    cam.on_position(-6.9, 107.6);
    cam.release();
    CHECK_FALSE(cam.following());
    CHECK_FALSE(cam.on_position(-6.91, 107.61).has_value());
    cam.relock();
    auto up = cam.on_position(-6.92, 107.62);
    REQUIRE(up.has_value());
    CHECK(up->zoom < 0);
    CHECK(up->lat == doctest::Approx(-6.92));
}

TEST_CASE("adjust_zoom clamps to [3, 19]") {
    FollowCamera cam(15.0);
    for (int i = 0; i < 30; ++i) cam.adjust_zoom(+1);
    CHECK(cam.zoom() == doctest::Approx(19.0));
    for (int i = 0; i < 30; ++i) cam.adjust_zoom(-1);
    CHECK(cam.zoom() == doctest::Approx(3.0));
}
