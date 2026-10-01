#include "camera.h"
#include <cmath>
#include <cstdio>
#include <initializer_list>

namespace {
int failures = 0;
const float response = Camera{}.turn_response_seconds;
void check(bool ok, const char* name) {
    std::printf("%s %s\n", ok ? "PASS" : "FAIL", name);
    failures += !ok;
}
bool near(float a, float b, float eps = 0.0001f) {
    return std::abs(a - b) < eps;
}
float run(int fps) {
    TurnResponse axis;
    float angle = 0;
    for (int phase = 0; phase < 3; ++phase) {
        const float target = phase == 0 ? 1.4f : phase == 1 ? -1.4f : 0.0f;
        for (int frame = 0; frame < fps; ++frame)
            angle += axis.step(target, 1.0f / fps, response);
    }
    return angle;
}
// Fly-by-wire slide (#643): sideways travel (velocity off the nose,
// integrated over time) while a top-speed ship hauls through a 90 degree
// turn and settles. Also reports the settled speed via out-param.
float slide_distance(float damping, float* settled_speed) {
    Camera ship;
    ship.linear_damping = damping;
    const float dt = 1.0f / 120;
    ship.set_forward_input(ship.max_speed_cruise0);
    for (int i = 0; i < 600; ++i) ship.integrate(dt);
    const HMM_Vec3 start = ship.forward();
    float slide = 0;
    for (int i = 0; i < 1200; ++i) {
        const bool turning = HMM_DotV3(ship.forward(), start) > 0;
        ship.apply_mouse_aim(turning ? 1.0f : 0.0f, 0, dt);
        ship.integrate(dt);
        const HMM_Vec3 nose = ship.forward();
        const HMM_Vec3 off_nose = HMM_SubV3(ship.velocity,
            HMM_MulV3F(nose, HMM_DotV3(ship.velocity, nose)));
        slide += HMM_LenV3(off_nose) * dt;
    }
    *settled_speed = HMM_LenV3(ship.velocity);
    return slide;
}
}

int main() {
    TurnResponse axis;
    const float first = axis.step(1.4f, 1.0f / 60, response);
    check(first > 0 && first < 1.4f / 60 * 0.1f, "turn begins gently, not at full rate");
    for (int i = 1; i < 38; ++i) axis.step(1.4f, 1.0f / 60, response);
    check(axis.rate > 1.4f * 0.89f && axis.rate < 1.4f, "heavier ramp: ~90% in 633ms");
    const float before = axis.rate;
    axis.step(-1.4f, 1.0f / 60, response);
    check(axis.rate > 0 && std::abs(axis.rate - before) < 0.15f, "reversal unwinds rather than snapping");
    bool bounded = true;
    for (int i = 0; i < 600; ++i) {
        axis.step(i % 37 < 18 ? 1.4f : -1.4f, 1.0f / 120, response);
        bounded &= std::abs(axis.rate) <= 1.40001f;
    }
    check(bounded, "rapid reversals stay inside hull rate limit");
    for (int i = 0; i < 360; ++i) axis.step(0, 1.0f / 120, response);
    check(std::abs(axis.rate) < 0.00001f, "recentering brakes to rest");
    check(near(run(30), run(60)) && near(run(60), run(144)), "integrated angle agrees at 30/60/144Hz");
    TurnResponse single, split;
    const float total = single.step(1.4f, 0.5f, response);
    float parts = 0;
    for (float dt : {0.03f, 0.12f, 0.10f, 0.25f}) parts += split.step(1.4f, dt, response);
    check(near(total, parts) && near(single.rate, split.rate), "hitch and uneven steps match exact solution");
    check(axis.step(1, 0, response) == 0 && axis.step(1, -1, response) == 0, "nonpositive time is a no-op");
    axis.reset();
    check(axis.rate == 0 && axis.drive == 0, "handoff reset clears inertia");
    check(near(axis.step(1.4f, 0.1f, 0), 0.14f), "zero response time supports instant legacy tuning");

    check(Camera{}.cockpit_head_motion_strength == 1.0f,
          "small rigid slide is enabled independently of turn inertia");
    Camera heavy;
    for (int i = 0; i < 9; ++i) heavy.apply_mouse_aim(1, 0, 1.0f / 60);
    const float fraction = -heavy.yaw_response.rate / heavy.max_yaw_rate;
    check(fraction > 0.20f && fraction < 0.30f,
          "actual camera has perceptible buildup: 20-30% rate at 150ms");

    Camera cam;
    cam.apply_mouse_aim(0.01f, -0.01f, 1.0f);
    check(near(cam.orientation.W, 1) && near(cam.orientation.X, 0), "camera deadzone is still neutral at rest");
    for (int i = 0; i < 600; ++i) cam.apply_mouse_aim(1, 1, 1.0f / 60);
    const auto q = cam.orientation;
    check(near(q.X*q.X + q.Y*q.Y + q.Z*q.Z + q.W*q.W, 1), "combined yaw/pitch keeps quaternion normalized");
    check(cam.yaw_response.rate < 0 && cam.pitch_response.rate > 0, "camera preserves yaw/pitch sign conventions");
    cam.reset_turn_response();
    const auto previous = cam.orientation;
    cam.apply_mouse_aim(0, 0, 1.0f / 60);
    check(near(previous.X, cam.orientation.X) && near(previous.W, cam.orientation.W), "no latent turn after control handoff");

    float old_speed = 0, new_speed = 0;
    const float old_slide = slide_distance(4.0f, &old_speed);  // pre-#643 tuning
    const float new_slide = slide_distance(Camera{}.linear_damping, &new_speed);
    const float slide_ratio = new_slide / old_slide;
    std::printf("     90deg-turn slide: %.1f -> %.1f (x%.3f)\n", old_slide, new_slide, slide_ratio);
    check(old_slide > 10.0f, "old tuning really slid sideways through a 90deg turn");
    check(slide_ratio > 0.45f && slide_ratio < 0.55f, "fly-by-wire slide is about half the pre-#643 amount");
    check(near(new_speed, Camera{}.max_speed_cruise0, 0.5f) && near(old_speed, new_speed, 0.5f),
          "halving slide leaves top speed untouched");
    return failures ? 1 : 0;
}
