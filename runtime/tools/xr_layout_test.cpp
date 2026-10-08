// VR screen and GamePad panel placement, and the controller's pointer on the panel (runtime/src/xr/layout.h).
#include <cassert>
#include <cmath>
#include <cstdio>

#include "xr/layout.h"

using namespace xr::layout;

static bool close_to(float a, float b, float eps = 1e-3f) { return std::fabs(a - b) <= eps; }
static bool close_to(Vec3 a, Vec3 b, float eps = 1e-3f) { return close_to(a.x, b.x, eps) && close_to(a.y, b.y, eps) && close_to(a.z, b.z, eps); }
static Vec3 unit(Vec3 v) { return v * (1.0f / std::sqrt(dot(v, v))); }
// how far left (+) or right (-) of the anchor's heading a point is seen from the anchor (radians)
static float seen_left(const Anchor& a, Vec3 point) {
    const Vec3 local = rotate(conj(about_y(a.yaw)), point - a.position);
    return std::atan2(-local.x, -local.z);
}
static float seen_up(const Anchor& a, Vec3 point) {
    const Vec3 local = rotate(conj(about_y(a.yaw)), point - a.position);
    return std::atan2(local.y, std::sqrt(local.x * local.x + local.z * local.z));
}

static void test_math() {
    // a heading turns to the left: 90 degrees looks along -X
    assert(close_to(rotate(about_y(kPi / 2), {0, 0, -1}), {-1, 0, 0}));
    assert(close_to(heading(about_y(0.7f), 0), 0.7f));
    assert(close_to(heading(about_y(-2.1f), 0), -2.1f));
    // the head's tilt does not change its heading; straight up keeps the fallback
    assert(close_to(heading(mul(about_y(0.7f), about_x(0.5f)), 0), 0.7f));
    assert(close_to(heading(about_x(kPi / 2), 1.25f), 1.25f));
    // about_x raises what is ahead
    assert(close_to(rotate(about_x(kPi / 2), {0, 0, -1}), {0, 1, 0}));
    const Quat q = mul(about_y(0.3f), about_x(-0.4f));
    assert(close_to(rotate(conj(q), rotate(q, {1, 2, 3})), {1, 2, 3}));
}

static void test_flat_screen() {
    const float aspect = 16.0f / 9.0f;
    Anchor a;
    Placement s = place_screen(a, 2.5f, 70, 0, 0, aspect, true);
    assert(!s.cylinder);
    assert(close_to(s.pose.p, {0, 0, -2.5f}));
    assert(close_to(s.width, 2 * 2.5f * std::tan(radians(35)), 1e-4f));
    assert(close_to(s.height, s.width / aspect, 1e-4f));
    // it faces the player: its front (+Z of the pose) looks back at the anchor
    assert(close_to(rotate(s.pose.q, {0, 0, 1}), {0, 0, 1}));
    // its edges are seen 35 degrees to each side
    assert(close_to(seen_left(a, s.pose.p + rotate(s.pose.q, {-s.width / 2, 0, 0})), radians(35)));
    assert(close_to(seen_left(a, s.pose.p + rotate(s.pose.q, {s.width / 2, 0, 0})), radians(-35)));

    // another anchor: 90 degrees to the left, somewhere else, the screen raised by 0.3 m
    a = {{1, 1.5f, -2}, kPi / 2};
    s = place_screen(a, 4, 90, 0, 0.3f, aspect, false);
    assert(close_to(s.pose.p, {1 - 4, 1.8f, -2}));
    assert(close_to(rotate(s.pose.q, {0, 0, 1}), {1, 0, 0}));  // towards the anchor
    assert(close_to(s.width, 8, 1e-3f));
    // a curve is flat on a headset without curved layers
    assert(!place_screen(a, 4, 90, 0.8f, 0, aspect, false).cylinder);
}

static void test_curved_screen() {
    const float aspect = 16.0f / 9.0f;
    const Anchor a{{0.2f, 1.6f, 0.4f}, -0.6f};
    for (float curve : {1.0f, 0.75f, 0.4f, 0.1f}) {
        for (float size : {40.0f, 70.0f, 120.0f}) {
            const float d = 2.5f;
            const Placement s = place_screen(a, d, size, curve, 0, aspect, true);
            assert(s.cylinder);
            assert(close_to(s.radius, d / curve, 1e-3f));
            assert(close_to(s.width, s.radius * s.angle, 1e-4f) && close_to(s.height, s.width / aspect, 1e-4f));
            // the arc's middle is `distance` ahead of the anchor, its ends are seen size / 2 to each side
            const Quat h = s.pose.q;
            const Vec3 middle = s.pose.p + rotate(h, {0, 0, -s.radius});
            assert(close_to(middle, a.position + rotate(about_y(a.yaw), {0, 0, -d}), 2e-3f));
            const Vec3 left = s.pose.p + rotate(h, {-s.radius * std::sin(s.angle / 2), 0, -s.radius * std::cos(s.angle / 2)});
            assert(close_to(seen_left(a, left), radians(size) / 2, 2e-3f));
            // never wider than a flat screen of that size, and equally far everywhere at curve 1
            assert(s.angle <= radians(size) + 1e-4f);
            if (curve == 1.0f) assert(close_to(s.angle, radians(size), 1e-3f) && close_to(s.pose.p, a.position, 1e-3f));
        }
    }
}

static void test_panel() {
    const float aspect = 16.0f / 9.0f, drc = 854.0f / 480.0f;
    // a controller held level and pointing ahead: the panel stands above it, faces back towards the
    // player and up, and its lower edge clears the controller
    {
        const Placement p = place_panel(Pose{}, drc);
        assert(!p.cylinder && close_to(p.width, kPanelWidth) && close_to(p.height, kPanelWidth / drc, 1e-4f));
        assert(close_to(p.pose.p.x, 0) && close_to(p.pose.p.y, kPanelRise));
        const Vec3 normal = rotate(p.pose.q, {0, 0, 1});
        assert(close_to(normal.x, 0) && close_to(normal.y, std::sin(radians(kPanelLean))) && close_to(normal.z, std::cos(radians(kPanelLean))));
        assert((p.pose.p + rotate(p.pose.q, {0, -p.height / 2, 0})).y > 0.03f);
        // a player's eyes 0.35 m behind and 0.3 m above the hand see its front, near its middle
        float u = -1, v = -1;
        const Vec3 eyes{0, 0.30f, 0.35f};
        assert(hit(p, eyes, unit(p.pose.p - eyes), &u, &v) && close_to(u, 0.5f) && close_to(v, 0.5f));
    }
    for (const Anchor& a : {Anchor{}, Anchor{{-1, 1.2f, 3}, 2.4f}}) {
        for (float size : {30.0f, 70.0f, 140.0f}) {
            const float d = 2.5f, h = 0.2f;
            // the hand: somewhere ahead of and below the anchor, the controller turned and tilted
            const Pose hand = in_anchor(a, {-0.2f, -0.35f, -0.4f}, mul(about_y(radians(size / 7)), about_x(radians(size / 5 - 10))));
            const Placement p = place_panel(hand, drc);
            assert(!p.cylinder && close_to(p.width, kPanelWidth) && close_to(p.height, kPanelWidth / drc, 1e-4f));
            // it moves and turns with the hand: where panel_in_hand says, in the hand's own space
            const Pose in = panel_in_hand();
            assert(close_to(rotate(conj(hand.q), p.pose.p - hand.p), in.p, 1e-4f));
            assert(close_to(rotate(p.pose.q, {0, 0, 1}), rotate(hand.q, rotate(in.q, {0, 0, 1})), 1e-4f));

            // the pointer: a ray from the head to the panel's centre, to its corners, past it, from behind
            float u = -1, v = -1;
            assert(hit(p, a.position, unit(p.pose.p - a.position), &u, &v) && close_to(u, 0.5f) && close_to(v, 0.5f));
            const Vec3 topLeft = p.pose.p + rotate(p.pose.q, {-p.width * 0.499f, p.height * 0.499f, 0});
            assert(hit(p, a.position, unit(topLeft - a.position), &u, &v) && close_to(u, 0, 5e-3f) && close_to(v, 0, 5e-3f));
            const Vec3 lowRight = p.pose.p + rotate(p.pose.q, {p.width * 0.25f, -p.height * 0.25f, 0});
            assert(hit(p, a.position, unit(lowRight - a.position), &u, &v) && close_to(u, 0.75f) && close_to(v, 0.75f));
            const Vec3 beside = p.pose.p + rotate(p.pose.q, {p.width, 0, 0});
            assert(!hit(p, a.position, unit(beside - a.position), &u, &v));
            assert(!hit(p, a.position, unit(a.position - p.pose.p), &u, &v));             // pointing away
            const Vec3 behind = p.pose.p + (p.pose.p - a.position);
            assert(!hit(p, behind, unit(p.pose.p - behind), &u, &v));                    // from its back
            // (the pointer is for flat layers: a curved one never reports a hit)
            assert(!hit(place_screen(a, d, size, 1, h, aspect, true), a.position, rotate(about_y(a.yaw), {0, 0, -1}), &u, &v));
        }
    }
}

static void test_aim() {
    float yaw, pitch, level;
    aim_angles({0, 0, -1}, &yaw, &pitch, &level);
    assert(close_to(yaw, 0) && close_to(pitch, 0) && close_to(level, 1));
    aim_angles({1, 0, 0}, &yaw, &pitch, &level);  // to the right
    assert(close_to(yaw, kPi / 2) && close_to(pitch, 0));
    aim_angles(unit({-1, 1, -1}), &yaw, &pitch, &level);  // ahead to the left, raised
    assert(close_to(yaw, -kPi / 4) && close_to(pitch, std::atan2(1.0f, std::sqrt(2.0f))));
    aim_angles({0, 1, 0}, &yaw, &pitch, &level);  // straight up: no heading
    assert(close_to(pitch, kPi / 2) && close_to(level, 0));
    // a controller turned to the right by 0.2 rad reads 0.2 more
    aim_angles(rotate(about_y(-0.5f), {0, 0, -1}), &yaw, &pitch, &level);
    float yaw2;
    aim_angles(rotate(about_y(-0.7f), {0, 0, -1}), &yaw2, &pitch, &level);
    assert(close_to(yaw2 - yaw, 0.2f));
}

// world mode: an eye of the headset as a camera in the game's world
static void test_eye_camera() {
    const float scale = 100;
    Vec3 heading{0, 0, -1};
    const Anchor seat{{0.5f, 1.2f, -0.3f}, 0.7f};
    const Pose level{about_y(seat.yaw), seat.position};  // the head at the anchor, facing the anchor's way
    // a game camera above and behind Link, looking down at him along +X
    const GameCamera game{{1000, 300, 2000}, {1250, 200, 2000}, {0, 1, 0}};
    const float distance = std::sqrt(250.0f * 250 + 100 * 100);

    // seated at the anchor: the camera's place, its heading, but a level horizon
    GameCamera c = eye_camera(game, heading, false, seat, level, scale);
    assert(close_to(c.eye, game.eye, 1e-2f));
    assert(close_to(heading, {1, 0, 0}));
    assert(close_to(c.center, game.eye + Vec3{1, 0, 0} * distance, 1e-2f));
    assert(close_to(c.up, {0, 1, 0}));
    // ... while aiming, the camera's own direction
    c = eye_camera(game, heading, true, seat, level, scale);
    assert(close_to(c.eye, game.eye, 1e-2f) && close_to(c.center, game.center, 1e-2f));
    assert(close_to(dot(c.up, c.center - c.eye), 0, 1e-2f) && c.up.y > 0.9f);

    // the head 0.5 m to the right of the anchor (the anchor's right, not the room's): looking along +X,
    // right is +Z in the game's right-handed world; 0.2 m up and 0.3 m forward as well
    Pose moved = level;
    moved.p = seat.position + rotate(about_y(seat.yaw), {0.5f, 0.2f, -0.3f});
    c = eye_camera(game, heading, false, seat, moved, scale);
    assert(close_to(c.eye, game.eye + Vec3{30, 20, 50}, 1e-2f));
    assert(close_to(c.center - c.eye, Vec3{1, 0, 0} * distance, 1e-2f));  // still looking the same way

    // the head turned 90 degrees to the left looks along -Z here (left of +X), 30 degrees up looks up
    Pose turned = level;
    turned.q = mul(about_y(seat.yaw), about_y(kPi / 2));
    c = eye_camera(game, heading, false, seat, turned, scale);
    assert(close_to((c.center - c.eye) * (1 / distance), {0, 0, -1}));
    turned.q = mul(about_y(seat.yaw), about_x(radians(30)));
    c = eye_camera(game, heading, false, seat, turned, scale);
    assert(close_to((c.center - c.eye) * (1 / distance), {std::cos(radians(30)), std::sin(radians(30)), 0}));
    // a head tilted 20 degrees to the right tilts the camera's up to its right (+Z here)
    turned.q = mul(about_y(seat.yaw), Quat{0, 0, -std::sin(radians(10)), std::cos(radians(10))});
    c = eye_camera(game, heading, false, seat, turned, scale);
    assert(close_to(c.up, {0, std::cos(radians(20)), std::sin(radians(20))}));

    // the two eyes: the right eye is to the right, both look the same way
    Pose left = level, right = level;
    left.p = seat.position + rotate(level.q, {-0.032f, 0, 0});
    right.p = seat.position + rotate(level.q, {0.032f, 0, 0});
    const GameCamera l = eye_camera(game, heading, false, seat, left, scale), r = eye_camera(game, heading, false, seat, right, scale);
    assert(close_to(r.eye - l.eye, {0, 0, 6.4f}, 1e-2f) && close_to(r.center - r.eye, l.center - l.eye, 1e-2f));

    // a camera looking straight down keeps the heading it had
    const GameCamera down{{0, 500, 0}, {0, 0, 0}, {1, 0, 0}};
    c = eye_camera(down, heading, false, seat, level, scale);
    assert(close_to(heading, {1, 0, 0}) && close_to(c.center - c.eye, Vec3{1, 0, 0} * 500, 1e-2f));
}

int main() {
    test_eye_camera();
    test_math();
    test_flat_screen();
    test_curved_screen();
    test_panel();
    test_aim();
    printf("xr_layout_test: ok\n");
    return 0;
}
