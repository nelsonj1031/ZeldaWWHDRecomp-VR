// World mode of the VR headset (world.h).
#include "world.h"

#include <algorithm>
#include <atomic>
#include <chrono>
#include <cmath>
#include <cstdio>
#include <cstdlib>
#include <cstring>

#include "aspect.h"
#include "gx2/gx2_cmd.h"
#include "interp.h"
#include "layout.h"
#include "runtime.h"

namespace interp { uint64_t logic_steps(); }

namespace xrworld {
namespace {
using namespace xr::layout;

constexpr float kHudDistance = 2.0f;   // metres: where the HUD floats
// The HUD follows where the head looks, late and smoothly (follow_hud): it stands still while the
// head stays within kHudStill of it, closes what is beyond that with a time constant of kHudLag
// seconds, and is never further behind than kHudLeash (yaw, pitch; degrees).
constexpr float kHudLag = 0.25f;
constexpr float kHudStill[2] = {4.0f, 3.0f}, kHudLeash[2] = {30.0f, 22.0f};
constexpr uint64_t kLinger = 12;       // passes without a camera before the screen takes over again
constexpr int kSettle = 4;             // eye pictures after a change of shape that are not shown
constexpr float kZoomed = 30.0f;       // degrees: a narrower camera is a zoom (the game plays at 40 and more)

// WWHD_VR_TEST_HEAD: a head without a headset
struct TestHead {
    bool on = false;
    float yaw = 0, pitch = 0, x = 0, y = 0, z = 0, eyes = 0.064f, roll = 0;
};
const TestHead& test_head() {
    static const TestHead head = [] {
        TestHead h;
        if (const char* e = getenv("WWHD_VR_TEST_HEAD")) {
            h.on = true;
            sscanf(e, "%f,%f,%f,%f,%f,%f,%f", &h.yaw, &h.pitch, &h.x, &h.y, &h.z, &h.eyes, &h.roll);
            LOG("[vr] WWHD_VR_TEST_HEAD: world mode with a test head (%.0f left, %.0f up, at %.2f %.2f %.2f m)", h.yaw, h.pitch, h.x, h.y, h.z);
        }
        return h;
    }();
    return head;
}
bool test_eyes(xr::EyeView eyes[2], float anchor[4]) {
    const TestHead& h = test_head();
    const Quat q = mul(mul(about_y(radians(h.yaw)), about_x(radians(h.pitch))), Quat{0, 0, std::sin(radians(h.roll) / 2), std::cos(radians(h.roll) / 2)});
    const float half = radians(50.0f);
    for (int e = 0; e < 2; e++) {
        const Vec3 p = Vec3{h.x, h.y, h.z} + rotate(q, {(e ? 0.5f : -0.5f) * h.eyes, 0, 0});
        eyes[e] = {{q.x, q.y, q.z, q.w}, {p.x, p.y, p.z}, {-half, half, half, -half}};
    }
    anchor[0] = anchor[1] = anchor[2] = anchor[3] = 0;
    return true;
}

// ---- game thread
struct Pair {
    bool valid = false;
    uint64_t step = 0;
    int pair = 0;
    xr::EyeView eyes[2];
    float anchor[4] = {};
};
// where the HUD stands: a direction in the headset's space (yaw to the left, pitch up; radians)
struct Hud {
    bool valid = false;
    float yaw = 0, pitch = 0;
    std::chrono::steady_clock::time_point at;
};
struct Game {
    Pair pair;
    Hud hud;
    float halfH = 0, halfV = 0;  // the field of view the pictures are drawn with (to each side, up and down)
    float aspect = 0;
    int settle = 0;              // frames left that are drawn but not shown
    Vec3 heading{0, 0, -1};      // the game camera's last level direction
} G;
std::atomic<uint64_t> g_lastEyeStep{~0ull};  // logic step of the latest eye picture
std::atomic<float> g_aspect{0}, g_hud{1};
std::atomic<bool> g_on{false};

bool sample(Pair& p) { return test_head().on ? test_eyes(p.eyes, p.anchor) : xr::locate_eyes(p.eyes, p.anchor); }

// The HUD is no part of the view: it is a panel in the room, kHudDistance ahead of the head. A HUD
// drawn at a fixed place of the eyes' pictures stutters whenever the head turns, because the
// headset turns each picture with the room until the next one comes, the HUD in it too, and the next
// picture has it back in its place. A panel that stands in the room is turned rightly. It follows
// where the head looks, once per frame (a pair of pictures): not at all for small movements, then
// smoothly.
// debug: WWHD_VR_TEST_HUD="yaw,pitch" (degrees to the left / up) keeps the HUD in that direction
void follow_hud(const Pair& p) {
    static const char* fixed = getenv("WWHD_VR_TEST_HUD");
    if (fixed) {
        float yaw = 0, pitch = 0;
        sscanf(fixed, "%f,%f", &yaw, &pitch);
        G.hud.valid = true;
        G.hud.yaw = radians(yaw);
        G.hud.pitch = radians(pitch);
        return;
    }
    const Vec3 ahead = rotate(Quat{p.eyes[0].q[0], p.eyes[0].q[1], p.eyes[0].q[2], p.eyes[0].q[3]}, {0, 0, -1});
    const float level = std::sqrt(ahead.x * ahead.x + ahead.z * ahead.z);
    const float to[2] = {level > 0.05f ? std::atan2(-ahead.x, -ahead.z) : G.hud.yaw, std::atan2(ahead.y, level)};
    const auto now = std::chrono::steady_clock::now();
    if (!G.hud.valid) {
        G.hud = {true, to[0], to[1], now};
        return;
    }
    const float dt = std::clamp(std::chrono::duration<float>(now - G.hud.at).count(), 0.0f, 0.1f);
    G.hud.at = now;
    const float step = 1 - std::exp(-dt / kHudLag);
    float* at[2] = {&G.hud.yaw, &G.hud.pitch};
    for (int i = 0; i < 2; i++) {
        float behind = to[i] - *at[i];  // how far the head is ahead of the HUD
        if (i == 0) behind = std::remainder(behind, 2 * kPi);
        const float still = radians(kHudStill[i]), leash = radians(kHudLeash[i]);
        behind -= (behind - std::clamp(behind, -still, still)) * step;
        *at[i] = to[i] - std::clamp(behind, -leash, leash);
    }
}
// The HUD's panel as an eye sees it: the 3x3 matrix that takes a layout's own (x, y, w), where the
// layout fills -1..1, to the eye's picture's (x, y, w). `scale`: the panel's size as a part of the
// view's.
void hud_matrix(const xr::EyeView& view, float scale, float g[9]) {
    const Quat eye{view.q[0], view.q[1], view.q[2], view.q[3]};
    const Vec3 head = (Vec3{G.pair.eyes[0].p[0], G.pair.eyes[0].p[1], G.pair.eyes[0].p[2]} + Vec3{G.pair.eyes[1].p[0], G.pair.eyes[1].p[1], G.pair.eyes[1].p[2]}) * 0.5f;
    const Quat panel = mul(conj(eye), mul(about_y(G.hud.yaw), about_x(G.hud.pitch)));  // (upright: the head's tilt is not the HUD's)
    const float th = std::tan(G.halfH), tv = std::tan(G.halfV);
    // in the eye's own space: the panel's half width and half height, and its centre
    const Vec3 a = rotate(panel, {1, 0, 0}) * (kHudDistance * th * scale), b = rotate(panel, {0, 1, 0}) * (kHudDistance * tv * scale);
    const Vec3 c = rotate(panel, {0, 0, -1}) * kHudDistance + rotate(conj(eye), head - Vec3{view.p[0], view.p[1], view.p[2]});
    const float k = 1 / kHudDistance;  // (w is 1 at the panel's centre, seen straight on)
    g[0] = a.x / th * k, g[1] = b.x / th * k, g[2] = c.x / th * k;
    g[3] = a.y / tv * k, g[4] = b.y / tv * k, g[5] = c.y / tv * k;
    g[6] = -a.z * k, g[7] = -b.z * k, g[8] = -c.z * k;
}

// the field of view both eyes are drawn with: the same to both sides and up and down, wide enough
// for either eye, and no narrower than tall (the game's projection takes aspect ratios from 1 up)
void set_fov(const xr::EyeView eyes[2]) {
    float h = 0, v = 0;
    for (int e = 0; e < 2; e++) {
        h = std::max({h, -eyes[e].fov[0], eyes[e].fov[1]});
        v = std::max({v, eyes[e].fov[2], -eyes[e].fov[3]});
    }
    h = std::clamp(h, radians(20.0f), radians(80.0f));
    v = std::clamp(v, radians(20.0f), radians(80.0f));
    // (below 16:9: at exactly 16:9 aspect.cpp leaves the game alone, the HUD's projections too)
    const float a = std::clamp(std::tan(h) / std::tan(v), 1.0f, 1.7f);
    h = std::atan(a * std::tan(v));
    if (std::fabs(a - G.aspect) > 0.01f || std::fabs(v - G.halfV) > radians(0.5f)) {
        LOG("[vr] world mode: an eye sees %.0f x %.0f degrees, aspect %.3f", 2 * h * 180 / kPi, 2 * v * 180 / kPi, a);
        G.aspect = a;
        G.halfH = h;
        G.halfV = v;
        G.settle = kSettle;
    }
}
}  // namespace

bool wanted() {
    if (!xr::options().world) return false;
    if (!test_head().on && !xr::running()) return false;
    // Two eyes are two passes of a logic step: frame interpolation is on, at 120 fps or more, so a
    // step has room for two pairs (60 pictures a second in the headset). interp.cpp plans each step
    // with as many pairs as the computer draws in time.
    if (interp::mode() != 1 || interp::fps() < 120) {
        if (interp::fps() < 120) interp::set_fps(120);
        interp::set_mode(1);
    }
    return true;
}
// (the screen takes over again when no camera was drawn for a while: a scene of menus)
bool active() { return g_on.load(std::memory_order_relaxed) && interp::logic_steps() <= g_lastEyeStep.load(std::memory_order_relaxed) + kLinger; }
int pair_phase(int phase) { return active() ? phase | 1 : phase; }
float aspect() { return active() ? g_aspect.load(std::memory_order_relaxed) : 0.0f; }
float hud_scale() { return active() ? g_hud.load(std::memory_order_relaxed) : 1.0f; }

bool eye_camera(int phase, bool aiming, float eye[3], float center[3], float up[3], float* fovy, int16_t* bank) {
    const uint64_t step = interp::logic_steps();
    if (!active()) g_on = false;
    // The telescope and the Picto Box zoom by narrowing the camera's field of view, which an eye's
    // view cannot do: zoomed pictures go to the screen.
    if (!wanted() || *fovy < kZoomed) {
        g_on = false;
        return false;
    }
    const int index = phase & 1, pair = phase >> 1;
    if (!G.pair.valid || G.pair.step != step || G.pair.pair != pair) {  // a new frame: where the head is
        if (!sample(G.pair)) {
            G.pair.valid = false;
            return false;
        }
        G.pair.valid = true;
        G.pair.step = step;
        G.pair.pair = pair;
        set_fov(G.pair.eyes);
        if (!g_on) G.hud.valid = false;  // (the HUD begins where the head looks)
        follow_hud(G.pair);
    }
    const xr::Options o = xr::options();
    const xr::EyeView& view = G.pair.eyes[index];
    if (!g_on) G.settle = kSettle;  // the game's projection and render targets take a few frames to follow
    g_on = true;
    g_lastEyeStep = step;
    g_aspect = G.aspect;
    g_hud = o.hud;

    // the eye as a camera in the game's world (layout.h): the anchor is where the player sat, facing the screen
    const GameCamera now = xr::layout::eye_camera({{eye[0], eye[1], eye[2]}, {center[0], center[1], center[2]}, {up[0], up[1], up[2]}}, G.heading, aiming,
                                                  Anchor{{G.pair.anchor[0], G.pair.anchor[1], G.pair.anchor[2]}, G.pair.anchor[3]},
                                                  Pose{{view.q[0], view.q[1], view.q[2], view.q[3]}, {view.p[0], view.p[1], view.p[2]}}, o.scale);
    eye[0] = now.eye.x, eye[1] = now.eye.y, eye[2] = now.eye.z;
    center[0] = now.center.x, center[1] = now.center.y, center[2] = now.center.z;
    up[0] = now.up.x, up[1] = now.up.y, up[2] = now.up.z;
    *bank = 0;  // (the head's tilt is in `up`)
    // (aspect.cpp gives the projection and the culling frustum this fovy with world mode's aspect ratio,
    // without its own widening for shapes narrower than 16:9)
    *fovy = 2 * G.halfV * 180 / kPi;

    // the frame's eye for the render thread: the pose as the headset gave it, the field of view as drawn
    const bool settled = G.settle == 0 && aspect::game() == G.aspect;
    if (G.settle > 0 && index == 1) G.settle--;
    const float separation = xr::layout::length(Vec3{G.pair.eyes[1].p[0], G.pair.eyes[1].p[1], G.pair.eyes[1].p[2]} -
                                                Vec3{G.pair.eyes[0].p[0], G.pair.eyes[0].p[1], G.pair.eyes[0].p[2]});
    // the HUD two metres away: each eye sees it shifted towards the nose by half the eye distance,
    // as a part of the picture's half width
    const float shift = (separation / 2) / (kHudDistance * std::tan(G.halfH));  // the left eye's; the right eye's is the opposite
    uint32_t w[24];
    w[0] = (uint32_t)index | (settled ? 2u : 0u);
    for (int i = 0; i < 4; i++) w[1 + i] = gx2::fbits(view.q[i]);
    for (int i = 0; i < 3; i++) w[5 + i] = gx2::fbits(view.p[i]);
    w[8] = gx2::fbits(-G.halfH);
    w[9] = gx2::fbits(G.halfH);
    w[10] = gx2::fbits(G.halfV);
    w[11] = gx2::fbits(-G.halfV);
    w[12] = gx2::fbits(o.hud);
    w[13] = gx2::fbits(shift);
    w[14] = 1;  // the HUD's panel follows
    float g[9];
    hud_matrix(view, o.hud, g);
    for (int i = 0; i < 9; i++) w[15 + i] = gx2::fbits(g[i]);
    gx2::vr_eye(w, 24);
    return true;
}

// ---- render thread
namespace {
// The game's main loop paints the draw lists of its previous pass, then draws the camera and builds
// the lists of this one, and swaps at the end: the camera whose OP_VR_EYE arrives between two swaps
// is the one of the picture that ends with the swap after the next (measured with the test head:
// tagged with the swap right after it, every picture carried the other eye's label). So an eye waits
// one swap: `drawing` is the picture being drawn now (its layouts' projections too), `next` the one
// whose camera has arrived.
struct Eye {
    Frame frame;
    float hud = 1, shift = 0;  // the HUD's size; its shift for the left eye (the right eye's is the opposite)
    bool placed = false;       // the HUD's panel as this eye sees it (hud_matrix)
    float panel[9] = {};
};
struct Render {
    Eye drawing, next;
} Rn;
}  // namespace

void eye_op(const uint32_t* w, uint32_t n) {
    if (n < 14) return;
    Eye& e = Rn.next;
    e.frame.eye = true;
    e.frame.index = (int)(w[0] & 1);
    e.frame.settled = (w[0] & 2) != 0;
    for (int i = 0; i < 4; i++) e.frame.view.q[i] = gx2::bitsf(w[1 + i]);
    for (int i = 0; i < 3; i++) e.frame.view.p[i] = gx2::bitsf(w[5 + i]);
    for (int i = 0; i < 4; i++) e.frame.view.fov[i] = gx2::bitsf(w[8 + i]);
    e.hud = gx2::bitsf(w[12]);
    e.shift = gx2::bitsf(w[13]);
    e.placed = n >= 24 && (w[14] & 1);
    if (e.placed)
        for (int i = 0; i < 9; i++) e.panel[i] = gx2::bitsf(w[15 + i]);
}
Frame take_frame() {
    const Frame f = Rn.drawing.frame;
    Rn.drawing = Rn.next;
    Rn.next = Eye{};
    return f;
}
void hud_projection(float* m) {
    const Eye& e = Rn.drawing;
    if (!e.frame.eye) return;
    // The layout's x, y, w (where it would be in the view, as the game draws it) go through the
    // panel's matrix. The depth cannot follow exactly (it would be z w' / w): it stays what it is
    // for a far plane, `plane`, and what a pane is nearer than that it stays in the panel's middle.
    // The game's layouts have a perspective projection whose depths are all close to that plane.
    if (e.placed) {
        const float* g = e.panel;
        const float plane = std::fabs(m[14]) > 1e-6f ? m[10] / m[14] : std::fabs(m[15]) > 1e-6f ? m[11] / m[15] : 0.0f;
        for (int i = 0; i < 4; i++) {
            const float x = m[i], y = m[4 + i], z = m[8 + i], w = m[12 + i];
            const float w2 = g[6] * x + g[7] * y + g[8] * w;
            m[i] = g[0] * x + g[1] * y + g[2] * w;
            m[4 + i] = g[3] * x + g[4] * y + g[5] * w;
            m[8 + i] = g[8] * z + plane * (w2 - g[8] * w);
            m[12 + i] = w2;
        }
        return;
    }
    // (any other projection) smaller about the view's centre, then sideways by `shift` of the half width (x += shift * w)
    const float shift = e.frame.index ? -e.shift : e.shift;
    for (int i = 0; i < 4; i++) {
        m[i] = m[i] * e.hud + shift * m[12 + i];
        m[4 + i] *= e.hud;
    }
}

}  // namespace xrworld
