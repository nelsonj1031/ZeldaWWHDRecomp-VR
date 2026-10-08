// VR headsets through OpenXR (PC VR: a Meta Quest over Link or Air Link, SteamVR, ...; docs/vr.md).
//
// What the player gets: the game's world around them in 3D (world mode, xr/world.h) or the picture
// of the TV window on a large screen in front of them, the GamePad picture on a panel they take out
// on their left controller, and the headset's controllers as one more host controller. Their buttons
// and sticks go through the controls mapping like a gamepad's (input_map.h), the game's rumble plays
// as haptics, the right controller can turn the virtual GamePad (gyro source "vr", motion.h) and
// touches the GamePad panel by pointing at it.
//
// Controllers (Touch layout; the mapping lives in xr.cpp, the controls mapping applies on top):
//   sticks, A B X Y by their letters, triggers = ZL / ZR, grips = L / R, right stick click = R3
//   Menu: a press is Plus; held for half a second it opens the settings overlay
//   left stick held down ("shift"): right stick = D-pad, Menu = Minus, right stick click recentres
//   left stick clicked (down and up at once): the GamePad panel appears on the left controller, or
//   goes away; the right controller points at it (trigger = touch)
//
// Threads: the Vulkan renderer owns the session's graphics (xr_vulkan.h, gfx/vulkan/xr_present.cpp)
// and runs the frame on the render thread, in swap(); the host's main loop reads the controllers
// (poll_controllers). Everything else here may be called from any thread.
//
// Without WWHD_OPENXR (CMake option) every function is an inline no-op: callers need no #ifdef.
#pragma once
#include <string>

namespace xr {

// settings.ini "vr*" keys (settings overlay > Display > VR headset)
struct Options {
    bool enabled = false;   // start in the headset ("vr"); WWHD_VR=0|1, --vr and --no-vr decide one start
    float distance = 2.5f;  // metres from the player to the screen ("vrDistance")
    float size = 70.0f;     // the screen's width as the player sees it, degrees ("vrSize")
    float curve = 0.0f;     // 0 flat .. 1 curved around the player ("vrCurve"; runtimes with cylinder layers)
    float height = 0.0f;    // the screen's centre above (+) or below (-) eye level, metres ("vrHeight")
    bool gamepad = true;    // a click of the left stick takes out the GamePad panel ("vrGamePad")
    bool haptics = true;    // the game's rumble on the controllers ("vrHaptics")
    // world mode (xr/world.h): the game world around the player in 3D instead of the screen
    bool world = true;      // "vrWorld"
    float scale = 100.0f;   // game units to the metre ("vrScale"): larger makes the world smaller
    float hud = 0.5f;       // the HUD's size as a part of the view's width ("vrHud")
    bool operator==(const Options&) const = default;
    static constexpr float kMinDistance = 1.0f, kMaxDistance = 8.0f, kMinSize = 30.0f, kMaxSize = 140.0f;
    static constexpr float kMinScale = 20.0f, kMaxScale = 400.0f, kMinHud = 0.25f, kMaxHud = 1.0f;
};

// One eye of the headset: its pose in the session's LOCAL space (OpenXR coordinates: metres, +Y up,
// -Z ahead; orientation x, y, z, w) and its field of view (radians: left and down are negative).
struct EyeView {
    float q[4] = {0, 0, 0, 1};
    float p[3] = {0, 0, 0};
    float fov[4] = {0, 0, 0, 0};  // left, right, up, down
};

#ifdef WWHD_OPENXR
inline bool compiled() { return true; }
Options options();
void set_options(const Options& o);  // applies at once and is saved; `enabled` counts at the next start
bool requested();                    // this start wants the headset
void request(bool on);               // --vr / --no-vr
bool active();                       // a session exists (the headset may be idle or not worn)
bool running();                      // the headset takes frames now
int display_hz();                    // the headset's refresh rate while running, else 0
bool curve_supported();
std::string status();                // one line for the settings overlay and the log
void recenter();                     // the screen moves in front of where the player looks now
// Orderly exit: the session and the connection to the headset's runtime end. No frame may be in
// progress (the renderer calls it with its render thread held). A runtime that still finds its
// session open when the process ends may abort inside its own teardown (Meta's does).
void shutdown();

// Host main loop, every update: the controllers, read at most every few milliseconds. `values`
// are input_map::kPadCount host controller inputs (0..1); the headset's are merged in (maximum).
// False: no session, or it has no input right now.
bool poll_controllers(float* values);
void note_other_controller();        // a gamepad was used (its button or stick is down)
// The headset's controllers were used last: confirm and cancel follow their letters in the overlay
// (they feed A to the east like a Nintendo pad, where menus elsewhere take the south button).
bool controllers_in_use();
void set_haptics(float level);       // 0..1 on both controllers until the next call (the game's rumble)
// render thread: where the right controller points on the GamePad panel (0..1 from its top left)
bool gamepad_pointer(float* u, float* v, bool* pressed);
void show_gamepad(bool on);          // the GamePad panel on the left controller (as the left stick's click does)
bool gamepad_shown();
// Game thread (world mode): both eyes for a picture that is drawn now and shown a few frames later,
// and the anchor the screen stands at (x, y, z, heading: where the player sat and looked when it was
// placed). False while the headset takes no frames or does not track.
bool locate_eyes(EyeView eyes[2], float anchor[4]);
#else
inline bool compiled() { return false; }
inline Options options() { return {}; }
inline void set_options(const Options&) {}
inline bool requested() { return false; }
inline void request(bool) {}
inline bool active() { return false; }
inline bool running() { return false; }
inline int display_hz() { return 0; }
inline bool curve_supported() { return false; }
inline std::string status() { return "This build has no VR support (CMake: -DWWHD_OPENXR=ON)."; }
inline void recenter() {}
inline void shutdown() {}
inline bool poll_controllers(float*) { return false; }
inline void note_other_controller() {}
inline bool controllers_in_use() { return false; }
inline void set_haptics(float) {}
inline bool gamepad_pointer(float*, float*, bool*) { return false; }
inline void show_gamepad(bool) {}
inline bool gamepad_shown() { return false; }
inline bool locate_eyes(EyeView*, float*) { return false; }
#endif

}  // namespace xr
