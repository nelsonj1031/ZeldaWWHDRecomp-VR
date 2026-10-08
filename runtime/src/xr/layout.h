// Where the VR screen stands and the GamePad panel is held, and where a controller points on them (xr.h).
//
// Plain C++ (no OpenXR), unit-tested in runtime/tools/xr_layout_test.cpp.
//
// Also world mode's camera (world.h): an eye of the headset as a camera in the game's world.
//
// Coordinates are OpenXR's: metres, right-handed, +Y up, -Z ahead; unit quaternions (x, y, z, w). The
// layers stand relative to an anchor, the player's head position and heading when the screen was placed
// (the head's tilt does not count: the screen stands upright). A heading (yaw) turns to the left:
// heading a looks along (-sin a, 0, -cos a).
//   screen  `distance` ahead of the anchor, `sizeDegrees` wide as seen from it; flat, or part of an
//           upright cylinder whose axis is behind the player (curve 1: the axis goes through the
//           anchor, the screen is equally far everywhere; smaller: a wider radius, a gentler curve)
//   panel   the GamePad picture in the player's left hand: a tablet standing on the controller
#pragma once
#include <algorithm>
#include <cmath>

namespace xr::layout {

constexpr float kPi = 3.14159265358979f;
inline float radians(float degrees) { return degrees * kPi / 180.0f; }

struct Vec3 {
    float x = 0, y = 0, z = 0;
};
struct Quat {
    float x = 0, y = 0, z = 0, w = 1;
};
struct Pose {
    Quat q;
    Vec3 p;
};
inline Vec3 operator+(Vec3 a, Vec3 b) { return {a.x + b.x, a.y + b.y, a.z + b.z}; }
inline Vec3 operator-(Vec3 a, Vec3 b) { return {a.x - b.x, a.y - b.y, a.z - b.z}; }
inline Vec3 operator*(Vec3 a, float s) { return {a.x * s, a.y * s, a.z * s}; }
inline float dot(Vec3 a, Vec3 b) { return a.x * b.x + a.y * b.y + a.z * b.z; }
inline Quat mul(const Quat& a, const Quat& b) {
    return {a.w * b.x + a.x * b.w + a.y * b.z - a.z * b.y, a.w * b.y - a.x * b.z + a.y * b.w + a.z * b.x,
            a.w * b.z + a.x * b.y - a.y * b.x + a.z * b.w, a.w * b.w - a.x * b.x - a.y * b.y - a.z * b.z};
}
inline Quat conj(const Quat& q) { return {-q.x, -q.y, -q.z, q.w}; }
inline Vec3 rotate(const Quat& q, Vec3 v) {
    const Vec3 t{2 * (q.y * v.z - q.z * v.y), 2 * (q.z * v.x - q.x * v.z), 2 * (q.x * v.y - q.y * v.x)};
    return {v.x + q.w * t.x + (q.y * t.z - q.z * t.y), v.y + q.w * t.y + (q.z * t.x - q.x * t.z),
            v.z + q.w * t.z + (q.x * t.y - q.y * t.x)};
}
inline Quat about_y(float a) { return {0, std::sin(a / 2), 0, std::cos(a / 2)}; }
inline Quat about_x(float a) { return {std::sin(a / 2), 0, 0, std::cos(a / 2)}; }

struct Anchor {
    Vec3 position;
    float yaw = 0;
};
// the heading of a head (or controller) orientation; `fallback` while it looks straight up or down
inline float heading(const Quat& orientation, float fallback) {
    const Vec3 ahead = rotate(orientation, {0, 0, -1});
    return ahead.x * ahead.x + ahead.z * ahead.z > 0.01f ? std::atan2(-ahead.x, -ahead.z) : fallback;
}
inline Pose in_anchor(const Anchor& a, Vec3 offset, const Quat& turn) {
    const Quat h = about_y(a.yaw);
    return {mul(h, turn), a.position + rotate(h, offset)};
}

struct Placement {
    Pose pose;                    // a flat layer's centre (it faces +Z of the pose), or a cylinder's centre
    float width = 0, height = 0;  // metres (a cylinder: along its arc)
    bool cylinder = false;
    float radius = 0, angle = 0;  // cylinder: metres, and the arc's angle seen from its axis (radians)
};

// aspect: the picture's width / height; cylinders: the headset can show curved layers
inline Placement place_screen(const Anchor& anchor, float distance, float sizeDegrees, float curve, float height, float aspect,
                              bool cylinders) {
    Placement p;
    const float half = radians(sizeDegrees) / 2;
    if (cylinders && curve > 0.02f) {
        // a cylinder of radius r whose axis is r - distance behind the player: the arc's angle a, for the
        // player to see it sizeDegrees wide (they are no further from the arc than its axis, so a <= size)
        const float r = distance / std::min(curve, 1.0f);
        float lo = 0, hi = 2 * half;
        for (int i = 0; i < 40; i++) {
            const float a = (lo + hi) / 2;
            (std::atan2(r * std::sin(a / 2), r * std::cos(a / 2) - (r - distance)) < half ? lo : hi) = a;
        }
        p.cylinder = true;
        p.radius = r;
        p.angle = (lo + hi) / 2;
        p.width = r * p.angle;
        p.height = p.width / aspect;
        p.pose = in_anchor(anchor, {0, height, r - distance}, {});
    } else {
        p.width = 2 * distance * std::tan(half);
        p.height = p.width / aspect;
        p.pose = in_anchor(anchor, {0, height, -distance}, {});
    }
    return p;
}

// The GamePad picture in the player's hand: a tablet that stands on the controller and leans back,
// so that it faces a player who looks down at their hand. `hand` is the controller's aim pose (-Z
// where it points, +Y up from it); panel_in_hand is the panel's pose in that space.
constexpr float kPanelWidth = 0.30f, kPanelRise = 0.13f;  // metres: the panel's width, its centre above the controller
constexpr float kPanelLean = 40.0f;                       // degrees back from upright
inline Pose panel_in_hand() { return {about_x(radians(-kPanelLean)), {0, kPanelRise, -0.02f}}; }
inline Placement place_panel(const Pose& hand, float aspect) {
    const Pose in = panel_in_hand();
    Placement p;
    p.width = kPanelWidth;
    p.height = kPanelWidth / aspect;
    p.pose = {mul(hand.q, in.q), hand.p + rotate(hand.q, in.p)};
    return p;
}

// Where a ray from `origin` along the unit vector `ahead` meets the front of a flat layer: u, v are 0..1
// from its top left. False when the ray misses it, comes from behind, or the layer is further than 10 m.
inline bool hit(const Placement& quad, Vec3 origin, Vec3 ahead, float* u, float* v) {
    if (quad.cylinder || quad.width <= 0 || quad.height <= 0) return false;
    const Vec3 normal = rotate(quad.pose.q, {0, 0, 1});
    const float towards = dot(ahead, normal);
    if (towards > -1e-3f) return false;
    const float t = dot(quad.pose.p - origin, normal) / towards;
    if (t <= 0 || t > 10) return false;
    const Vec3 at = rotate(conj(quad.pose.q), origin + ahead * t - quad.pose.p);
    *u = at.x / quad.width + 0.5f;
    *v = 0.5f - at.y / quad.height;
    return *u >= 0 && *u <= 1 && *v >= 0 && *v <= 1;
}

// A pointing direction as angles (radians): yaw to the right of -Z, pitch above the horizon; level is
// its horizontal length (near 0 the yaw means nothing).
inline void aim_angles(Vec3 ahead, float* yaw, float* pitch, float* level) {
    *level = std::sqrt(ahead.x * ahead.x + ahead.z * ahead.z);
    *yaw = std::atan2(ahead.x, -ahead.z);
    *pitch = std::atan2(ahead.y, *level);
}

// ---- world mode: an eye as a camera in the game's world
inline float length(Vec3 v) { return std::sqrt(dot(v, v)); }
inline Vec3 cross(Vec3 a, Vec3 b) { return {a.y * b.z - a.z * b.y, a.z * b.x - a.x * b.z, a.x * b.y - a.y * b.x}; }
struct GameCamera {
    Vec3 eye, center, up{0, 1, 0};
};
// The player stands where the game's camera is and faces the way it faces. `camera` is the game's own
// (its world is right-handed with +Y up, like OpenXR's); `eye` is an eye's pose in the headset's
// space and `anchor` the place and heading there that stand for the game camera. The eye's offset
// from the anchor, `unitsPerMetre` game units to the metre, is added to the camera's position; its
// direction goes on top of the camera's heading. The camera's own pitch and roll are left out, so
// the horizon stays level however the game tilts its camera, except while `aiming`: then the whole
// direction counts, and a head looking straight ahead looks where the game aims.
// `heading` keeps the camera's last level direction for when it looks straight up or down.
inline GameCamera eye_camera(const GameCamera& camera, Vec3& heading, bool aiming, const Anchor& anchor, const Pose& eye, float unitsPerMetre) {
    const Quat unturn = about_y(-anchor.yaw);
    const Vec3 place = rotate(unturn, eye.p - anchor.position);
    const Quat turn = mul(unturn, eye.q);
    const Vec3 ahead = rotate(turn, {0, 0, -1}), above = rotate(turn, {0, 1, 0});
    const Vec3 look = camera.center - camera.eye;
    const float distance = std::max(length(look), 1.0f);
    if (const float level = std::sqrt(look.x * look.x + look.z * look.z); level > 0.05f * distance) heading = {look.x / level, 0, look.z / level};
    Vec3 f = heading, u{0, 1, 0};
    if (aiming && length(look) > 1e-3f) {
        f = look * (1.0f / length(look));
        Vec3 w = camera.up - f * dot(camera.up, f);
        if (length(w) < 0.05f) w = Vec3{0, 1, 0} - f * f.y;
        if (length(w) > 1e-3f) u = w * (1.0f / length(w));
    }
    const Vec3 r = cross(f, u);
    auto to_game = [&](Vec3 v) { return r * v.x + u * v.y + f * -v.z; };
    GameCamera out;
    out.eye = camera.eye + to_game(place) * unitsPerMetre;
    out.center = out.eye + to_game(ahead) * distance;
    out.up = to_game(above);
    return out;
}

}  // namespace xr::layout
