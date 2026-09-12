// cyclomp — mock ride driver.
// Feeds fake but plausible telemetry into the Slint UI on a repeating timer,
// so the dashboard looks alive without any real sensors.

#include "cyclomp.h"

#include <chrono>
#include <cmath>
#include <cstdint>
#include <cstdio>

namespace {

// Tiny deterministic PRNG (no <random> pulled in for a mock).
struct Rng {
    uint32_t s = 0x9e3779b9u;
    float next() { // [0,1)
        s ^= s << 13; s ^= s >> 17; s ^= s << 5;
        return (s >> 8) * (1.0f / 16777216.0f);
    }
    float centered() { return next() - 0.5f; } // [-0.5,0.5)
};

struct RideModel {
    Rng rng;
    int   elapsed_s = 0;   // ride seconds
    float dist_km   = 0.f;
    float speed     = 0.f; // km/h
    float sum_speed = 0.f;
    int   samples   = 0;
    int   nav_m     = 400; // metres to next turn

    void tick(float dt_s, bool riding) {
        if (!riding) return;
        elapsed_s += (int)std::lround(dt_s);

        // Speed random-walk, clamped to a sane cycling range.
        speed += rng.centered() * 6.f;
        if (speed < 8.f)  speed = 8.f;
        if (speed > 42.f) speed = 42.f;

        dist_km   += speed * (dt_s / 3600.f);
        sum_speed += speed;
        samples   += 1;

        nav_m -= (int)std::lround(speed * (dt_s / 3600.f) * 1000.f);
        if (nav_m <= 0) nav_m = 400 + (int)(rng.next() * 800.f);
    }

    float avg() const { return samples ? sum_speed / samples : 0.f; }
    int   cadence() const { return riding_cadence(); }
    int   riding_cadence() const { return speed > 0 ? (int)std::lround(speed * 2.4f + 12) : 0; }
};

slint::SharedString fmt1(float v) { // one decimal
    char buf[32];
    std::snprintf(buf, sizeof buf, "%.1f", v);
    return slint::SharedString(buf);
}

slint::SharedString mmss(int s) {
    char buf[16];
    std::snprintf(buf, sizeof buf, "%02d:%02d", s / 60, s % 60);
    return slint::SharedString(buf);
}

slint::SharedString metres(int m) {
    char buf[24];
    if (m >= 1000) std::snprintf(buf, sizeof buf, "%.1f km", m / 1000.f);
    else           std::snprintf(buf, sizeof buf, "%d m", m);
    return slint::SharedString(buf);
}

} // namespace

#ifdef __ANDROID__
extern "C" void slint_main()
#else
int main(int, char **)
#endif
{
    auto ui = AppWindow::create();
    auto model = std::make_shared<RideModel>();

    ui->on_toggle_ride([ui = slint::ComponentWeakHandle(ui)] {
        if (auto u = ui.lock()) (*u)->set_riding(!(*u)->get_riding());
    });

    // ~500 ms telemetry tick.
    static slint::Timer timer;
    timer.start(slint::TimerMode::Repeated, std::chrono::milliseconds(500),
        [ui = slint::ComponentWeakHandle(ui), model] {
            auto u = ui.lock();
            if (!u) return;
            auto &w = **u;
            bool riding = w.get_riding();
            model->tick(0.5f, riding);

            w.set_speed(model->speed);
            w.set_distance(fmt1(model->dist_km));
            w.set_ride_time(mmss(model->elapsed_s));
            w.set_avg_speed(model->avg());
            w.set_cadence(model->cadence());

            // Heart rate tracks effort (speed) with jitter; zone 1..5.
            int hr = (int)std::lround(90 + model->speed * 1.9f + model->rng.centered() * 6.f);
            if (!riding) hr = 72;
            w.set_heart_rate(hr);
            w.set_hr_zone(hr < 105 ? 1 : hr < 125 ? 2 : hr < 145 ? 3 : hr < 165 ? 4 : 5);

            w.set_nav_distance(metres(model->nav_m));
        });

    ui->run();
#ifndef __ANDROID__
    return 0;
#endif
}
