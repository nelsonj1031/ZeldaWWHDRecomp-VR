// VR headsets through OpenXR (xr.h, xr_vulkan.h): instance and session, the screen and GamePad panel
// as composition layers, the controllers.
//
// The screen is a quad (or a cylinder, when curved) in a LOCAL reference space, so the headset's
// compositor draws it at its own refresh rate with the head's latest pose: the screen stands still
// however many frames a second the game presents. It is placed relative to an "anchor", the
// player's head position and heading when the session's first frame was drawn (or at the last
// recentre); layout.h says where. The GamePad panel is a quad in the left controller's own space:
// the compositor moves it with the hand.
#include "xr.h"
#include "xr_vulkan.h"
#include "layout.h"

#include <algorithm>
#include <atomic>
#include <chrono>
#include <cmath>
#include <cstdlib>
#include <condition_variable>
#include <cstring>
#include <mutex>
#include <thread>

#define XR_USE_GRAPHICS_API_VULKAN
#include <openxr/openxr.h>
#include <openxr/openxr_platform.h>

#include "input.h"
#include "input_map.h"
#include "motion/motion.h"
#include "overlay/hostui.h"
#include "runtime.h"

namespace xr {
namespace {

using Clock = std::chrono::steady_clock;
using layout::kPi;
double secs(Clock::duration d) { return std::chrono::duration<double>(d).count(); }

// layout.h's types and OpenXR's (the same coordinates)
layout::Vec3 from(const XrVector3f& v) { return {v.x, v.y, v.z}; }
layout::Quat from(const XrQuaternionf& q) { return {q.x, q.y, q.z, q.w}; }
XrPosef to_xr(const layout::Pose& p) { return {{p.q.x, p.q.y, p.q.z, p.q.w}, {p.p.x, p.p.y, p.p.z}}; }
const XrPosef kIdentity{{0, 0, 0, 1}, {0, 0, 0}};

// ---- state
struct Chain {
    XrSwapchain handle = XR_NULL_HANDLE;
    uint32_t width = 0, height = 0;
    std::vector<XrSwapchainImageVulkanKHR> images;
    uint32_t index = 0;
    bool held = false;        // acquired and not yet released
    bool ready = false;       // holds a picture: released at least once
    uint32_t pw = 0, ph = 0;  // the picture's part of the image, from its top left
    uint64_t drawn = 0;       // the frame (S.serial) it was last drawn in
};
enum Hand { kLeft, kRight };
struct Actions {
    XrActionSet set = XR_NULL_HANDLE;
    XrAction a = XR_NULL_HANDLE, b = XR_NULL_HANDLE, x = XR_NULL_HANDLE, y = XR_NULL_HANDLE, menu = XR_NULL_HANDLE;
    XrAction stick[2]{}, click[2]{}, trigger[2]{}, grip[2]{};
    XrAction aim = XR_NULL_HANDLE, haptic = XR_NULL_HANDLE;
    XrPath hand[2]{};
    XrSpace aimSpace[2]{};
};
struct State {
    XrInstance instance = XR_NULL_HANDLE;
    XrSystemId system = XR_NULL_SYSTEM_ID;
    XrSession session = XR_NULL_HANDLE;
    XrSpace local = XR_NULL_HANDLE, view = XR_NULL_HANDLE;
    XrSessionState state = XR_SESSION_STATE_UNKNOWN;
    XrEnvironmentBlendMode blend = XR_ENVIRONMENT_BLEND_MODE_OPAQUE;
    int64_t format = 0;
    uint32_t maxWidth = 0, maxHeight = 0;
    bool cylinder = false, refreshRates = false;  // extensions the runtime offers
    std::string runtime, headset, why;
    // extension functions (the loader exports the core ones only)
    PFN_xrGetVulkanInstanceExtensionsKHR getInstanceExtensions = nullptr;
    PFN_xrGetVulkanDeviceExtensionsKHR getDeviceExtensions = nullptr;
    PFN_xrGetVulkanGraphicsDeviceKHR getGraphicsDevice = nullptr;
    PFN_xrGetVulkanGraphicsRequirementsKHR getGraphicsRequirements = nullptr;
    PFN_xrEnumerateDisplayRefreshRatesFB enumerateRefreshRates = nullptr;
    PFN_xrGetDisplayRefreshRateFB getRefreshRate = nullptr;
    PFN_xrRequestDisplayRefreshRateFB requestRefreshRate = nullptr;
    void (*releaseHook)() = nullptr;
    // frame (render thread)
    Chain chains[kLayers];
    XrFrameState frame{XR_TYPE_FRAME_STATE};
    bool frameOpen = false;
    uint64_t serial = 0;             // counts frames
    uint32_t eyeWidth = 0, eyeHeight = 0;  // the image size the runtime recommends for an eye
    EyeView eyes[2];                 // world mode: what each eye's picture was drawn with
    layout::Anchor anchor;
    std::atomic<bool> anchored{false};
    // controllers (main thread; mu also guards the session against its end on the render thread)
    std::mutex mu;
    Actions actions;
    Clock::time_point polled{}, hapticSent{}, menuSince{}, plusUntil{}, homeUntil{}, shiftSince{};
    float pad[input_map::kPadCount] = {};
    bool padActive = false, menuDown = false, menuFired = false, recentreDown = false;
    bool shiftDown = false, shiftUsed = false;
    bool hapticOn = false, touching = false, triggerDown = false;
    bool aimValid = false;
    float aimYaw = 0, aimPitch = 0;
    bool pointing = false, pointerPressed = false;
    float pointerU = 0, pointerV = 0;
};
State S;
std::atomic<bool> g_active{false}, g_running{false}, g_recentre{false};
std::atomic<int> g_hz{0};
std::atomic<int64_t> g_displayTime{0}, g_period{0};
std::atomic<int64_t> g_displayAt{0};  // steady clock ticks when the headset named g_displayTime
std::atomic<float> g_haptics{0};
std::atomic<int64_t> g_usedAt{0}, g_otherAt{0};  // steady clock ticks of the last controller use
std::atomic<int> g_forced{-1};
std::atomic<bool> g_padOut{false};                  // the player has the GamePad panel out
std::atomic<float> g_padAspect{854.0f / 480.0f};    // its picture's shape

std::string result_text(XrResult r) {
    char text[XR_MAX_RESULT_STRING_SIZE] = "";
    if (S.instance && XR_SUCCEEDED(xrResultToString(S.instance, r, text))) return text;
    return "XrResult " + std::to_string((int)r);
}
// a failing call is logged the first few times (a frame call fails every frame once it does)
std::atomic<int> g_failures{0};
bool ok(XrResult r, const char* what) {
    if (XR_SUCCEEDED(r)) return true;
    if (g_failures++ < 12) LOG("[vr] %s failed: %s", what, result_text(r).c_str());
    return false;
}

// ---- options
std::mutex g_optionsMu;
Options g_options;
bool g_optionsRead = false;
void read_options_locked() {
    if (g_optionsRead) return;
    g_optionsRead = true;
    std::string v;
    auto num = [&](const char* key, float lo, float hi, float& out) {
        if (!hostui::get(key, v)) return;
        char* end = nullptr;
        const float x = strtof(v.c_str(), &end);
        if (end != v.c_str() && x == x) out = std::clamp(x, lo, hi);
    };
    if (hostui::get("vr", v)) g_options.enabled = v == "1";
    num("vrDistance", Options::kMinDistance, Options::kMaxDistance, g_options.distance);
    num("vrSize", Options::kMinSize, Options::kMaxSize, g_options.size);
    num("vrCurve", 0.0f, 1.0f, g_options.curve);
    num("vrHeight", -2.0f, 2.0f, g_options.height);
    if (hostui::get("vrGamePad", v)) g_options.gamepad = v != "0";
    if (hostui::get("vrHaptics", v)) g_options.haptics = v != "0";
    if (hostui::get("vrWorld", v)) g_options.world = v == "1";
    num("vrScale", Options::kMinScale, Options::kMaxScale, g_options.scale);
    num("vrHud", Options::kMinHud, Options::kMaxHud, g_options.hud);
    if (const char* e = getenv("WWHD_VR_WORLD")) g_options.world = atoi(e) != 0;
    // for one start (not saved unless changed in the overlay): WWHD_VR_SIZE, WWHD_VR_DISTANCE, WWHD_VR_CURVE
    auto env = [](const char* name, float lo, float hi, float& out) {
        if (const char* e = getenv(name); e && *e) out = std::clamp((float)atof(e), lo, hi);
    };
    env("WWHD_VR_SIZE", Options::kMinSize, Options::kMaxSize, g_options.size);
    env("WWHD_VR_DISTANCE", Options::kMinDistance, Options::kMaxDistance, g_options.distance);
    env("WWHD_VR_CURVE", 0.0f, 1.0f, g_options.curve);
}

// ---- the headset's frame clock
// xrWaitFrame blocks until the headset wants the next frame begun: up to a refresh period. Called
// where a frame begins, on the render thread, that wait comes on top of the drawing whenever a
// frame takes longer than a period, and the frame rate falls to a whole part of the refresh rate
// (a pair of eye pictures drawn in 35 ms: 24 a second at 72 Hz, not 29). So a thread of its own
// waits for the next frame as soon as one has begun, which OpenXR allows while that one is being
// drawn, and the render thread takes what it got: it only waits when it is ahead of the headset.
struct FrameClock {
    std::mutex mu;
    std::condition_variable cv;
    bool started = false;
    bool wanted = false, waiting = false, have = false;
    XrSession session = XR_NULL_HANDLE;
    XrResult result = XR_SUCCESS;
    XrFrameState state{XR_TYPE_FRAME_STATE};
    Clock::time_point at{};  // when the wait came back
};
FrameClock& C = *new FrameClock;  // (its thread never ends)
void clock_main() {
    std::unique_lock lk(C.mu);
    for (;;) {
        C.cv.wait(lk, [] { return C.wanted; });
        C.wanted = false;
        C.waiting = true;
        const XrSession session = C.session;
        lk.unlock();
        XrFrameWaitInfo wait{XR_TYPE_FRAME_WAIT_INFO};
        XrFrameState state{XR_TYPE_FRAME_STATE};
        const XrResult r = xrWaitFrame(session, &wait, &state);
        lk.lock();
        C.waiting = false;
        C.result = r;
        C.state = state;
        C.at = Clock::now();
        C.have = true;
        C.cv.notify_all();
    }
}
// render thread: the wait for the next frame begins now, unless it runs or is done
void clock_request() {
    std::lock_guard lk(C.mu);
    if (!C.started) {
        C.started = true;
        std::thread(clock_main).detach();
    }
    if (C.wanted || C.waiting || C.have) return;
    C.session = S.session;
    C.wanted = true;
    C.cv.notify_all();
}
// render thread: the next frame, when the headset wants it begun
XrResult clock_take(XrFrameState& state, Clock::time_point& at) {
    clock_request();
    std::unique_lock lk(C.mu);
    C.cv.wait(lk, [] { return C.have; });
    C.have = false;
    state = C.state;
    at = C.at;
    return C.result;
}
// render thread, where the frames end (the session stops or goes): no wait runs any more, none is kept
void clock_reset() {
    std::unique_lock lk(C.mu);
    C.wanted = false;
    if (!C.cv.wait_for(lk, std::chrono::seconds(2), [] { return !C.waiting; })) LOG("[vr] the headset's frame clock does not answer");
    C.have = false;
}

// ---- session end
void end_session() {
    if (!S.session) return;
    if (S.releaseHook) S.releaseHook();
    clock_reset();
    std::lock_guard lk(S.mu);
    if (S.touching) input::set_touch(false, 0, 0);
    S.touching = false;
    xrDestroySession(S.session);  // with its swapchains and spaces
    S.session = XR_NULL_HANDLE;
    S.local = S.view = XR_NULL_HANDLE;
    for (Chain& c : S.chains) c = Chain{};
    S.actions.aimSpace[0] = S.actions.aimSpace[1] = XR_NULL_HANDLE;
    S.frameOpen = false;
    std::fill(std::begin(S.pad), std::end(S.pad), 0.0f);
    S.padActive = S.pointing = false;
    g_active = false;
    g_running = false;
    g_hz = 0;
}
void leave(const char* why) {
    if (!S.session) return;
    LOG("[vr] leaving the headset: %s. The game goes on in its window; start it again for the headset.", why);
    end_session();
    S.why = why;
}

const char* state_name(XrSessionState s) {
    switch (s) {
    case XR_SESSION_STATE_IDLE: return "idle";
    case XR_SESSION_STATE_READY: return "ready";
    case XR_SESSION_STATE_SYNCHRONIZED: return "synchronized (not shown)";
    case XR_SESSION_STATE_VISIBLE: return "visible (no input: a system menu has it)";
    case XR_SESSION_STATE_FOCUSED: return "focused";
    case XR_SESSION_STATE_STOPPING: return "stopping";
    case XR_SESSION_STATE_LOSS_PENDING: return "loss pending";
    case XR_SESSION_STATE_EXITING: return "exiting";
    default: return "unknown";
    }
}

void poll_events() {
    XrEventDataBuffer event{XR_TYPE_EVENT_DATA_BUFFER};
    while (S.instance && xrPollEvent(S.instance, &event) == XR_SUCCESS) {
        switch (event.type) {
        case XR_TYPE_EVENT_DATA_SESSION_STATE_CHANGED: {
            const auto& e = *reinterpret_cast<const XrEventDataSessionStateChanged*>(&event);
            if (e.session != S.session) break;
            S.state = e.state;
            LOG("[vr] session %s", state_name(e.state));
            if (e.state == XR_SESSION_STATE_READY) {
                XrSessionBeginInfo info{XR_TYPE_SESSION_BEGIN_INFO};
                info.primaryViewConfigurationType = XR_VIEW_CONFIGURATION_TYPE_PRIMARY_STEREO;
                if (ok(xrBeginSession(S.session, &info), "xrBeginSession")) g_running = true;
            } else if (e.state == XR_SESSION_STATE_STOPPING) {
                g_running = false;
                g_hz = 0;
                ok(xrEndSession(S.session), "xrEndSession");
                clock_reset();  // (after it: a wait that runs comes back now, and belongs to no frame)
                S.frameOpen = false;
            } else if (e.state == XR_SESSION_STATE_EXITING) {
                leave("the headset closed the game's session");
            } else if (e.state == XR_SESSION_STATE_LOSS_PENDING) {
                leave("the headset's runtime is going away");
            }
            break;
        }
        case XR_TYPE_EVENT_DATA_INSTANCE_LOSS_PENDING: leave("the OpenXR runtime is shutting down"); break;
        case XR_TYPE_EVENT_DATA_REFERENCE_SPACE_CHANGE_PENDING:
            S.anchored = false;  // the headset was recentred: the screen follows
            break;
        case XR_TYPE_EVENT_DATA_INTERACTION_PROFILE_CHANGED: {
            if (!S.session) break;
            XrInteractionProfileState profile{XR_TYPE_INTERACTION_PROFILE_STATE};
            char name[XR_MAX_PATH_LENGTH] = "none";
            uint32_t n = 0;
            if (XR_SUCCEEDED(xrGetCurrentInteractionProfile(S.session, S.actions.hand[kRight], &profile)) && profile.interactionProfile != XR_NULL_PATH)
                xrPathToString(S.instance, profile.interactionProfile, sizeof name, &n, name);
            LOG("[vr] controllers: %s", name);
            break;
        }
        default: break;
        }
        event = {XR_TYPE_EVENT_DATA_BUFFER};
    }
}

// the anchor: the player's head position and heading now (the head's tilt does not count)
void set_anchor() {
    XrSpaceLocation head{XR_TYPE_SPACE_LOCATION};
    if (XR_FAILED(xrLocateSpace(S.view, S.local, S.frame.predictedDisplayTime, &head))) return;
    const XrSpaceLocationFlags need = XR_SPACE_LOCATION_ORIENTATION_VALID_BIT | XR_SPACE_LOCATION_POSITION_VALID_BIT;
    if ((head.locationFlags & need) != need) return;  // not tracking yet: the next frame tries again
    {
        std::lock_guard lk(S.mu);  // (locate_eyes reads it on the game thread)
        S.anchor.position = from(head.pose.position);
        S.anchor.yaw = layout::heading(from(head.pose.orientation), S.anchor.yaw);
    }
    S.anchored = true;
    LOG("[vr] screen placed ahead of the player (heading %.0f degrees)", S.anchor.yaw * 180 / kPi);
}

bool make_chain(Chain& c, uint32_t width, uint32_t height) {
    XrSwapchainCreateInfo info{XR_TYPE_SWAPCHAIN_CREATE_INFO};
    info.usageFlags = XR_SWAPCHAIN_USAGE_COLOR_ATTACHMENT_BIT | XR_SWAPCHAIN_USAGE_SAMPLED_BIT;
    info.format = S.format;
    info.sampleCount = 1;
    info.width = S.maxWidth ? std::min(width, S.maxWidth) : width;
    info.height = S.maxHeight ? std::min(height, S.maxHeight) : height;
    info.faceCount = info.arraySize = info.mipCount = 1;
    if (!ok(xrCreateSwapchain(S.session, &info, &c.handle), "xrCreateSwapchain")) return false;
    uint32_t n = 0;
    xrEnumerateSwapchainImages(c.handle, 0, &n, nullptr);
    c.images.assign(n, {XR_TYPE_SWAPCHAIN_IMAGE_VULKAN_KHR});
    if (!n || !ok(xrEnumerateSwapchainImages(c.handle, n, &n, reinterpret_cast<XrSwapchainImageBaseHeader*>(c.images.data())), "xrEnumerateSwapchainImages")) {
        xrDestroySwapchain(c.handle);
        c = Chain{};
        return false;
    }
    c.width = info.width;
    c.height = info.height;
    return true;
}

// ---- controllers
XrPath path(const char* text) {
    XrPath p = XR_NULL_PATH;
    xrStringToPath(S.instance, text, &p);
    return p;
}
bool make_actions() {
    Actions& a = S.actions;
    XrActionSetCreateInfo si{XR_TYPE_ACTION_SET_CREATE_INFO};
    snprintf(si.actionSetName, sizeof si.actionSetName, "gamepad");
    snprintf(si.localizedActionSetName, sizeof si.localizedActionSetName, "Wii U GamePad");
    if (!ok(xrCreateActionSet(S.instance, &si, &a.set), "xrCreateActionSet")) return false;
    a.hand[kLeft] = path("/user/hand/left");
    a.hand[kRight] = path("/user/hand/right");
    bool good = true;
    auto make = [&](XrAction& out, XrActionType type, const char* name, const char* label, bool bothHands = false) {
        XrActionCreateInfo ci{XR_TYPE_ACTION_CREATE_INFO};
        ci.actionType = type;
        snprintf(ci.actionName, sizeof ci.actionName, "%s", name);
        snprintf(ci.localizedActionName, sizeof ci.localizedActionName, "%s", label);
        if (bothHands) {
            ci.countSubactionPaths = 2;
            ci.subactionPaths = a.hand;
        }
        good = ok(xrCreateAction(a.set, &ci, &out), name) && good;
    };
    make(a.a, XR_ACTION_TYPE_BOOLEAN_INPUT, "a", "A");
    make(a.b, XR_ACTION_TYPE_BOOLEAN_INPUT, "b", "B");
    make(a.x, XR_ACTION_TYPE_BOOLEAN_INPUT, "x", "X");
    make(a.y, XR_ACTION_TYPE_BOOLEAN_INPUT, "y", "Y");
    make(a.menu, XR_ACTION_TYPE_BOOLEAN_INPUT, "menu", "Plus / settings");
    make(a.stick[kLeft], XR_ACTION_TYPE_VECTOR2F_INPUT, "left_stick", "Left stick (move)");
    make(a.stick[kRight], XR_ACTION_TYPE_VECTOR2F_INPUT, "right_stick", "Right stick (camera)");
    make(a.click[kLeft], XR_ACTION_TYPE_BOOLEAN_INPUT, "left_stick_click", "Shift (D-pad, Minus, GamePad pointer)");
    make(a.click[kRight], XR_ACTION_TYPE_BOOLEAN_INPUT, "right_stick_click", "Right stick click");
    make(a.trigger[kLeft], XR_ACTION_TYPE_FLOAT_INPUT, "left_trigger", "ZL");
    make(a.trigger[kRight], XR_ACTION_TYPE_FLOAT_INPUT, "right_trigger", "ZR");
    make(a.grip[kLeft], XR_ACTION_TYPE_FLOAT_INPUT, "left_grip", "L");
    make(a.grip[kRight], XR_ACTION_TYPE_FLOAT_INPUT, "right_grip", "R");
    make(a.aim, XR_ACTION_TYPE_POSE_INPUT, "aim", "Aim", true);
    make(a.haptic, XR_ACTION_TYPE_VIBRATION_OUTPUT, "rumble", "Rumble", true);
    if (!good) return false;
    // Touch controllers (Quest, Rift): the profile every Meta runtime offers
    const XrActionSuggestedBinding touch[] = {
        {a.a, path("/user/hand/right/input/a/click")},
        {a.b, path("/user/hand/right/input/b/click")},
        {a.x, path("/user/hand/left/input/x/click")},
        {a.y, path("/user/hand/left/input/y/click")},
        {a.menu, path("/user/hand/left/input/menu/click")},
        {a.stick[kLeft], path("/user/hand/left/input/thumbstick")},
        {a.stick[kRight], path("/user/hand/right/input/thumbstick")},
        {a.click[kLeft], path("/user/hand/left/input/thumbstick/click")},
        {a.click[kRight], path("/user/hand/right/input/thumbstick/click")},
        {a.trigger[kLeft], path("/user/hand/left/input/trigger/value")},
        {a.trigger[kRight], path("/user/hand/right/input/trigger/value")},
        {a.grip[kLeft], path("/user/hand/left/input/squeeze/value")},
        {a.grip[kRight], path("/user/hand/right/input/squeeze/value")},
        {a.aim, path("/user/hand/left/input/aim/pose")},
        {a.aim, path("/user/hand/right/input/aim/pose")},
        {a.haptic, path("/user/hand/left/output/haptic")},
        {a.haptic, path("/user/hand/right/output/haptic")},
    };
    XrInteractionProfileSuggestedBinding suggested{XR_TYPE_INTERACTION_PROFILE_SUGGESTED_BINDING};
    suggested.interactionProfile = path("/interaction_profiles/oculus/touch_controller");
    suggested.countSuggestedBindings = (uint32_t)std::size(touch);
    suggested.suggestedBindings = touch;
    return ok(xrSuggestInteractionProfileBindings(S.instance, &suggested), "xrSuggestInteractionProfileBindings (Touch controllers)");
}
bool attach_actions() {
    Actions& a = S.actions;
    XrSessionActionSetsAttachInfo attach{XR_TYPE_SESSION_ACTION_SETS_ATTACH_INFO};
    attach.countActionSets = 1;
    attach.actionSets = &a.set;
    if (!ok(xrAttachSessionActionSets(S.session, &attach), "xrAttachSessionActionSets")) return false;
    for (int h = 0; h < 2; h++) {
        XrActionSpaceCreateInfo space{XR_TYPE_ACTION_SPACE_CREATE_INFO};
        space.action = a.aim;
        space.subactionPath = a.hand[h];
        space.poseInActionSpace = kIdentity;
        ok(xrCreateActionSpace(S.session, &space, &a.aimSpace[h]), "xrCreateActionSpace");
    }
    return true;
}

// one read of the controllers (S.mu held): S.pad, the gyro, the GamePad pointer, haptics
void read_controllers(Clock::time_point now) {
    using namespace input_map;
    Actions& a = S.actions;
    float v[kPadCount] = {};
    bool any = false;
    XrActiveActionSet active{a.set, XR_NULL_PATH};
    XrActionsSyncInfo sync{XR_TYPE_ACTIONS_SYNC_INFO};
    sync.countActiveActionSets = 1;
    sync.activeActionSets = &active;
    // XR_SESSION_NOT_FOCUSED (a system menu has the input) is a success code: nothing is held then
    const bool focused = xrSyncActions(S.session, &sync) == XR_SUCCESS;
    XrActionStateGetInfo get{XR_TYPE_ACTION_STATE_GET_INFO};
    auto button = [&](XrAction action) {
        XrActionStateBoolean s{XR_TYPE_ACTION_STATE_BOOLEAN};
        get.action = action;
        if (!focused || XR_FAILED(xrGetActionStateBoolean(S.session, &get, &s)) || !s.isActive) return false;
        any = true;
        return s.currentState == XR_TRUE;
    };
    auto value = [&](XrAction action) {
        XrActionStateFloat s{XR_TYPE_ACTION_STATE_FLOAT};
        get.action = action;
        if (!focused || XR_FAILED(xrGetActionStateFloat(S.session, &get, &s)) || !s.isActive) return 0.0f;
        any = true;
        return std::clamp(s.currentState, 0.0f, 1.0f);
    };
    auto stick = [&](XrAction action) {
        XrActionStateVector2f s{XR_TYPE_ACTION_STATE_VECTOR2F};
        get.action = action;
        if (!focused || XR_FAILED(xrGetActionStateVector2f(S.session, &get, &s)) || !s.isActive) return XrVector2f{0, 0};
        any = true;
        return s.currentState;
    };
    const bool shift = button(a.click[kLeft]);
    // The face buttons by their letters, as on a Nintendo pad (A to the east): with the default
    // controls Touch A is the game's A, B its B, X its X and Y its Y, as the game's prompts say.
    v[kPadB] = button(a.a);
    v[kPadA] = button(a.b);
    v[kPadY] = button(a.x);
    v[kPadX] = button(a.y);
    v[kPadLT] = value(a.trigger[kLeft]);
    const float trigger = value(a.trigger[kRight]);
    v[kPadLB] = value(a.grip[kLeft]);
    v[kPadRB] = value(a.grip[kRight]);
    const XrVector2f left = stick(a.stick[kLeft]), right = stick(a.stick[kRight]);
    v[kPadLSRight] = std::max(left.x, 0.0f);
    v[kPadLSLeft] = std::max(-left.x, 0.0f);
    v[kPadLSUp] = std::max(left.y, 0.0f);
    v[kPadLSDown] = std::max(-left.y, 0.0f);
    const bool rightClick = button(a.click[kRight]), menu = button(a.menu);
    if (!shift) {
        v[kPadRSRight] = std::max(right.x, 0.0f);
        v[kPadRSLeft] = std::max(-right.x, 0.0f);
        v[kPadRSUp] = std::max(right.y, 0.0f);
        v[kPadRSDown] = std::max(-right.y, 0.0f);
        v[kPadR3] = rightClick;
        // Menu: a press is Plus (sent when it is released); held for half a second it is Home, which
        // opens the settings overlay
        if (menu && !S.menuDown) {
            S.menuSince = now;
            S.menuFired = false;
        }
        if (menu && !S.menuFired && secs(now - S.menuSince) >= 0.5) {
            S.menuFired = true;
            S.homeUntil = now + std::chrono::milliseconds(120);
        }
        if (!menu && S.menuDown && !S.menuFired) S.plusUntil = now + std::chrono::milliseconds(120);
    } else {
        // shift: the right stick is the D-pad (one direction at a time), Menu is Minus, the right
        // stick's click recentres the screen
        if (std::fabs(right.x) > std::fabs(right.y)) {
            v[kPadDRight] = right.x > 0.5f;
            v[kPadDLeft] = right.x < -0.5f;
        } else {
            v[kPadDUp] = right.y > 0.5f;
            v[kPadDDown] = right.y < -0.5f;
        }
        v[kPadOptions] = menu;
        if (rightClick && !S.recentreDown) g_recentre = true;
        S.menuFired = true;  // a Menu press that began or ends with shift held is no Plus
    }
    S.menuDown = menu;
    S.recentreDown = rightClick && shift;
    // The left stick's click by itself, down and up within a moment and nothing shifted in between,
    // takes the GamePad panel out on the left controller or puts it away.
    if (shift && !S.shiftDown) {
        S.shiftSince = now;
        S.shiftUsed = false;
    }
    if (shift && (rightClick || menu || std::max(std::fabs(right.x), std::fabs(right.y)) > 0.5f)) S.shiftUsed = true;
    if (focused && !shift && S.shiftDown && !S.shiftUsed && secs(now - S.shiftSince) < 0.4 && options().gamepad) {
        const bool out = !g_padOut.load();
        g_padOut = out;
        LOG("[vr] GamePad screen %s", out ? "on the left controller" : "put away");
    }
    S.shiftDown = shift;
    v[kPadMenu] = now < S.plusUntil;
    v[kPadHome] = now < S.homeUntil;

    // the right controller's aim: the gyro (motion.h, source "vr") follows how it turns, and it
    // points at the GamePad panel while that is out on the left controller
    bool pointing = false;
    float u = 0, w = 0;
    XrSpaceLocation aim{XR_TYPE_SPACE_LOCATION};
    const XrTime time = g_displayTime.load(std::memory_order_relaxed);
    const XrSpaceLocationFlags need = XR_SPACE_LOCATION_ORIENTATION_VALID_BIT | XR_SPACE_LOCATION_POSITION_VALID_BIT;
    const bool located = focused && time && a.aimSpace[kRight] && XR_SUCCEEDED(xrLocateSpace(a.aimSpace[kRight], S.local, time, &aim)) &&
                         (aim.locationFlags & need) == need;
    if (located) {
        const layout::Vec3 ahead = layout::rotate(from(aim.pose.orientation), {0, 0, -1});
        float yaw, pitch, level;
        layout::aim_angles(ahead, &yaw, &pitch, &level);
        if (S.aimValid && level > 0.3f) {
            const float turned = std::remainder(yaw - S.aimYaw, 2 * kPi), raised = pitch - S.aimPitch;
            // (a jump is the tracking finding the controller again, not a turn)
            if (std::fabs(turned) < 0.5f && std::fabs(raised) < 0.5f) motion::vr_turn(turned, raised);
        }
        if (level > 0.3f) S.aimYaw = yaw;
        S.aimPitch = pitch;
        S.aimValid = true;
        XrSpaceLocation hand{XR_TYPE_SPACE_LOCATION};
        if (gamepad_shown() && a.aimSpace[kLeft] && XR_SUCCEEDED(xrLocateSpace(a.aimSpace[kLeft], S.local, time, &hand)) &&
            (hand.locationFlags & need) == need) {
            const layout::Placement panel =
                layout::place_panel({from(hand.pose.orientation), from(hand.pose.position)}, g_padAspect.load(std::memory_order_relaxed));
            pointing = layout::hit(panel, from(aim.pose.position), ahead, &u, &w);
        }
    } else {
        S.aimValid = false;
    }
    // the trigger touches the GamePad while the pointer is on the panel, and is ZR otherwise
    S.triggerDown = trigger > (S.triggerDown ? 0.4f : 0.6f);
    const bool touch = pointing && S.triggerDown;
    if (touch) {
        input::set_touch(true, u, w);
    } else if (S.touching) {
        input::set_touch(false, S.pointerU, S.pointerV);
    }
    S.touching = touch;
    S.pointing = pointing;
    S.pointerPressed = touch;
    if (pointing) {
        S.pointerU = u;
        S.pointerV = w;
    } else {
        v[kPadRT] = trigger;
    }

    std::copy(std::begin(v), std::end(v), std::begin(S.pad));
    S.padActive = any;
    for (int p = 1; p < kPadCount; p++)
        if (v[p] > 0.5f) g_usedAt = now.time_since_epoch().count();

    // haptics: the level is sent again before a pulse runs out, and stopped when it drops to zero
    const float level = focused && options().haptics ? g_haptics.load(std::memory_order_relaxed) : 0.0f;
    XrHapticActionInfo info{XR_TYPE_HAPTIC_ACTION_INFO};
    info.action = a.haptic;
    if (level > 0.01f) {
        if (secs(now - S.hapticSent) >= 0.03) {
            S.hapticSent = now;
            XrHapticVibration pulse{XR_TYPE_HAPTIC_VIBRATION};
            pulse.amplitude = std::min(level, 1.0f);
            pulse.duration = 80'000'000;  // 80 ms, in nanoseconds
            pulse.frequency = XR_FREQUENCY_UNSPECIFIED;
            for (XrPath hand : a.hand) {
                info.subactionPath = hand;
                xrApplyHapticFeedback(S.session, &info, reinterpret_cast<const XrHapticBaseHeader*>(&pulse));
            }
            S.hapticOn = true;
        }
    } else if (S.hapticOn) {
        S.hapticOn = false;
        for (XrPath hand : a.hand) {
            info.subactionPath = hand;
            xrStopHapticFeedback(S.session, &info);
        }
    }
}

std::vector<std::string> split(const std::string& text) {
    std::vector<std::string> out;
    size_t at = 0;
    while (at < text.size()) {
        const size_t end = std::min(text.find(' ', at), text.size());
        if (end > at) out.push_back(text.substr(at, end - at));
        at = end + 1;
    }
    return out;
}
// "a b c" from one of the runtime's Vulkan extension queries
template <class Fn> std::vector<std::string> extension_list(Fn query, const char* what) {
    uint32_t n = 0;
    if (!query || !ok(query(S.instance, S.system, 0, &n, nullptr), what) || !n) return {};
    std::string text(n, '\0');
    if (!ok(query(S.instance, S.system, n, &n, text.data()), what)) return {};
    text.resize(strlen(text.c_str()));
    return split(text);
}

}  // namespace

// ---------------------------------------------------------------- options
Options options() {
    std::lock_guard lk(g_optionsMu);
    read_options_locked();
    return g_options;
}
void set_options(const Options& in) {
    Options o = in;
    o.distance = std::clamp(o.distance, Options::kMinDistance, Options::kMaxDistance);
    o.size = std::clamp(o.size, Options::kMinSize, Options::kMaxSize);
    o.curve = std::clamp(o.curve, 0.0f, 1.0f);
    o.height = std::clamp(o.height, -2.0f, 2.0f);
    o.scale = std::clamp(o.scale, Options::kMinScale, Options::kMaxScale);
    o.hud = std::clamp(o.hud, Options::kMinHud, Options::kMaxHud);
    Options old;
    {
        std::lock_guard lk(g_optionsMu);
        read_options_locked();
        old = g_options;
        if (o == old) return;
        g_options = o;
    }
    // (each one rewrites the settings file: only what changed, a slider sends many values)
    auto num = [](const char* key, float x, float was) {
        char b[32];
        snprintf(b, sizeof b, "%g", x);
        if (x != was) hostui::set(key, b);
    };
    auto flag = [](const char* key, bool on, bool was) {
        if (on != was) hostui::set(key, on ? "1" : "0");
    };
    flag("vr", o.enabled, old.enabled);
    num("vrDistance", o.distance, old.distance);
    num("vrSize", o.size, old.size);
    num("vrCurve", o.curve, old.curve);
    num("vrHeight", o.height, old.height);
    flag("vrGamePad", o.gamepad, old.gamepad);
    flag("vrHaptics", o.haptics, old.haptics);
    flag("vrWorld", o.world, old.world);
    num("vrScale", o.scale, old.scale);
    num("vrHud", o.hud, old.hud);
}
void request(bool on) { g_forced = on ? 1 : 0; }
bool requested() {
    if (const int f = g_forced.load(); f >= 0) return f != 0;
    if (const char* e = getenv("WWHD_VR")) return atoi(e) != 0;
    return options().enabled;
}
void shutdown() {
    if (!S.instance) return;
    end_session();
    std::lock_guard lk(S.mu);
    xrDestroyInstance(S.instance);  // with the action set
    S.instance = XR_NULL_HANDLE;
    S.actions = Actions{};
    S.why = "the game is closing";
    LOG("[vr] headset session closed");
}
bool active() { return g_active.load(std::memory_order_relaxed); }
bool running() { return g_running.load(std::memory_order_relaxed); }
int display_hz() { return g_hz.load(std::memory_order_relaxed); }
bool curve_supported() { return S.cylinder; }
void recenter() { g_recentre = true; }
void set_haptics(float level) { g_haptics.store(level, std::memory_order_relaxed); }
void note_other_controller() { g_otherAt = Clock::now().time_since_epoch().count(); }
bool controllers_in_use() {
    const int64_t used = g_usedAt.load(std::memory_order_relaxed);
    return active() && used && used >= g_otherAt.load(std::memory_order_relaxed);
}
std::string status() {
    if (active()) {
        std::string s = S.headset + " (" + S.runtime + ")";
        if (!running()) return s + ": waiting for the headset (put it on, or start Link in it).";
        if (const int hz = display_hz()) s += ", " + std::to_string(hz) + " Hz";
        return s + (S.state == XR_SESSION_STATE_FOCUSED ? "." : ": not in focus (a system menu is open, or the headset is not worn).");
    }
    if (!S.why.empty()) return "Not in the headset: " + S.why + ".";
    return requested() ? "Not in the headset." : "Off. Turn it on and start the game again to play in the headset.";
}

// ---------------------------------------------------------------- start-up
void abandon(const char* why) {
    LOG("[vr] no headset this time: %s", why);
    S.why = why;
    if (S.session) {
        xrDestroySession(S.session);
        S.session = XR_NULL_HANDLE;
    }
    if (S.instance) {
        xrDestroyInstance(S.instance);
        S.instance = XR_NULL_HANDLE;
    }
    g_active = false;
    g_running = false;
}

bool start() {
    uint32_t n = 0;
    XrResult r = xrEnumerateInstanceExtensionProperties(nullptr, 0, &n, nullptr);
    if (XR_FAILED(r)) {
        S.why = "no OpenXR runtime is set up on this computer (for a Quest: install Meta Quest Link and make it the active OpenXR runtime in its settings)";
        LOG("[vr] %s (%s)", S.why.c_str(), result_text(r).c_str());
        return false;
    }
    std::vector<XrExtensionProperties> offered(n, {XR_TYPE_EXTENSION_PROPERTIES});
    xrEnumerateInstanceExtensionProperties(nullptr, n, &n, offered.data());
    auto has = [&](const char* name) {
        return std::any_of(offered.begin(), offered.end(), [&](const XrExtensionProperties& e) { return !strcmp(e.extensionName, name); });
    };
    if (!has(XR_KHR_VULKAN_ENABLE_EXTENSION_NAME)) {
        S.why = "the OpenXR runtime does not take Vulkan applications (XR_KHR_vulkan_enable)";
        LOG("[vr] %s", S.why.c_str());
        return false;
    }
    std::vector<const char*> extensions{XR_KHR_VULKAN_ENABLE_EXTENSION_NAME};
    if ((S.cylinder = has(XR_KHR_COMPOSITION_LAYER_CYLINDER_EXTENSION_NAME))) extensions.push_back(XR_KHR_COMPOSITION_LAYER_CYLINDER_EXTENSION_NAME);
    if ((S.refreshRates = has(XR_FB_DISPLAY_REFRESH_RATE_EXTENSION_NAME))) extensions.push_back(XR_FB_DISPLAY_REFRESH_RATE_EXTENSION_NAME);
    XrInstanceCreateInfo ci{XR_TYPE_INSTANCE_CREATE_INFO};
    snprintf(ci.applicationInfo.applicationName, sizeof ci.applicationInfo.applicationName, "Wind Waker HD");
    snprintf(ci.applicationInfo.engineName, sizeof ci.applicationInfo.engineName, "ZeldaWWHDRecomp");
    ci.applicationInfo.applicationVersion = 1;
    ci.applicationInfo.apiVersion = XR_API_VERSION_1_0;  // every runtime has it; nothing here needs 1.1
    ci.enabledExtensionCount = (uint32_t)extensions.size();
    ci.enabledExtensionNames = extensions.data();
    r = xrCreateInstance(&ci, &S.instance);
    if (XR_FAILED(r)) {
        S.instance = XR_NULL_HANDLE;
        S.why = "the OpenXR runtime did not start (" + result_text(r) + ")";
        LOG("[vr] %s", S.why.c_str());
        return false;
    }
    XrInstanceProperties ip{XR_TYPE_INSTANCE_PROPERTIES};
    if (XR_SUCCEEDED(xrGetInstanceProperties(S.instance, &ip))) {
        char v[64];
        snprintf(v, sizeof v, " %u.%u.%u", XR_VERSION_MAJOR(ip.runtimeVersion), XR_VERSION_MINOR(ip.runtimeVersion), XR_VERSION_PATCH(ip.runtimeVersion));
        S.runtime = std::string(ip.runtimeName) + v;
    }
    XrSystemGetInfo gi{XR_TYPE_SYSTEM_GET_INFO};
    gi.formFactor = XR_FORM_FACTOR_HEAD_MOUNTED_DISPLAY;
    r = xrGetSystem(S.instance, &gi, &S.system);
    if (XR_FAILED(r)) {
        const std::string runtime = S.runtime;
        abandon(r == XR_ERROR_FORM_FACTOR_UNAVAILABLE ? "no headset is connected (a Quest: start Link or Air Link in the headset first)"
                                                      : ("the runtime has no headset (" + result_text(r) + ")").c_str());
        LOG("[vr] runtime: %s", runtime.c_str());
        return false;
    }
    XrSystemProperties sp{XR_TYPE_SYSTEM_PROPERTIES};
    if (XR_SUCCEEDED(xrGetSystemProperties(S.instance, S.system, &sp))) {
        S.headset = sp.systemName;
        S.maxWidth = sp.graphicsProperties.maxSwapchainImageWidth;
        S.maxHeight = sp.graphicsProperties.maxSwapchainImageHeight;
    }
    auto load = [&](const char* name, auto& fn) { return XR_SUCCEEDED(xrGetInstanceProcAddr(S.instance, name, reinterpret_cast<PFN_xrVoidFunction*>(&fn))) && fn; };
    if (!load("xrGetVulkanInstanceExtensionsKHR", S.getInstanceExtensions) || !load("xrGetVulkanDeviceExtensionsKHR", S.getDeviceExtensions) ||
        !load("xrGetVulkanGraphicsDeviceKHR", S.getGraphicsDevice) || !load("xrGetVulkanGraphicsRequirementsKHR", S.getGraphicsRequirements)) {
        abandon("the OpenXR runtime's Vulkan functions are missing");
        return false;
    }
    if (S.refreshRates)
        S.refreshRates = load("xrEnumerateDisplayRefreshRatesFB", S.enumerateRefreshRates) && load("xrGetDisplayRefreshRateFB", S.getRefreshRate) &&
                         load("xrRequestDisplayRefreshRateFB", S.requestRefreshRate);
    // (a session may only be created after this call)
    XrGraphicsRequirementsVulkanKHR req{XR_TYPE_GRAPHICS_REQUIREMENTS_VULKAN_KHR};
    if (!ok(S.getGraphicsRequirements(S.instance, S.system, &req), "xrGetVulkanGraphicsRequirementsKHR")) {
        abandon("the OpenXR runtime's Vulkan requirements are not available");
        return false;
    }
    LOG("[vr] %s on %s; Vulkan %u.%u to %u.%u; curved screen %s", S.headset.c_str(), S.runtime.c_str(), XR_VERSION_MAJOR(req.minApiVersionSupported),
        XR_VERSION_MINOR(req.minApiVersionSupported), XR_VERSION_MAJOR(req.maxApiVersionSupported), XR_VERSION_MINOR(req.maxApiVersionSupported),
        S.cylinder ? "available" : "not offered");
    return true;
}

bool started() { return S.instance != XR_NULL_HANDLE; }
std::vector<std::string> vulkan_instance_extensions() {
    return S.instance ? extension_list(S.getInstanceExtensions, "xrGetVulkanInstanceExtensionsKHR") : std::vector<std::string>{};
}
std::vector<std::string> vulkan_device_extensions() {
    return S.instance ? extension_list(S.getDeviceExtensions, "xrGetVulkanDeviceExtensionsKHR") : std::vector<std::string>{};
}
VkPhysicalDevice vulkan_physical_device(VkInstance instance) {
    VkPhysicalDevice device = VK_NULL_HANDLE;
    if (!S.instance || !ok(S.getGraphicsDevice(S.instance, S.system, instance, &device), "xrGetVulkanGraphicsDeviceKHR")) return VK_NULL_HANDLE;
    return device;
}
void set_images_release_hook(void (*hook)()) { S.releaseHook = hook; }

bool create_session(VkInstance instance, VkPhysicalDevice physicalDevice, VkDevice device, uint32_t queueFamily, uint32_t queueIndex) {
    if (!S.instance) return false;
    XrGraphicsBindingVulkanKHR binding{XR_TYPE_GRAPHICS_BINDING_VULKAN_KHR};
    binding.instance = instance;
    binding.physicalDevice = physicalDevice;
    binding.device = device;
    binding.queueFamilyIndex = queueFamily;
    binding.queueIndex = queueIndex;
    XrSessionCreateInfo si{XR_TYPE_SESSION_CREATE_INFO};
    si.next = &binding;
    si.systemId = S.system;
    XrResult r = xrCreateSession(S.instance, &si, &S.session);
    if (XR_FAILED(r)) {
        S.session = XR_NULL_HANDLE;
        abandon(("the headset did not open a session (" + result_text(r) + ")").c_str());
        return false;
    }
    XrReferenceSpaceCreateInfo space{XR_TYPE_REFERENCE_SPACE_CREATE_INFO};
    space.poseInReferenceSpace = kIdentity;
    space.referenceSpaceType = XR_REFERENCE_SPACE_TYPE_LOCAL;
    bool good = ok(xrCreateReferenceSpace(S.session, &space, &S.local), "xrCreateReferenceSpace (local)");
    space.referenceSpaceType = XR_REFERENCE_SPACE_TYPE_VIEW;
    good = ok(xrCreateReferenceSpace(S.session, &space, &S.view), "xrCreateReferenceSpace (view)") && good;
    // the layers' image format: sRGB (the compositor works in linear light), else whatever comes first
    uint32_t n = 0;
    xrEnumerateSwapchainFormats(S.session, 0, &n, nullptr);
    std::vector<int64_t> formats(n);
    xrEnumerateSwapchainFormats(S.session, n, &n, formats.data());
    S.format = 0;
    for (VkFormat want : {VK_FORMAT_R8G8B8A8_SRGB, VK_FORMAT_B8G8R8A8_SRGB, VK_FORMAT_R8G8B8A8_UNORM, VK_FORMAT_B8G8R8A8_UNORM})
        if (!S.format && std::find(formats.begin(), formats.end(), (int64_t)want) != formats.end()) S.format = want;
    if (!good || !S.format) {
        abandon(S.format ? "the headset's tracking spaces are not available" : "the headset offers no 8-bit RGBA image format");
        return false;
    }
    uint32_t modes = 0;
    XrEnvironmentBlendMode mode = XR_ENVIRONMENT_BLEND_MODE_OPAQUE;
    if (XR_SUCCEEDED(xrEnumerateEnvironmentBlendModes(S.instance, S.system, XR_VIEW_CONFIGURATION_TYPE_PRIMARY_STEREO, 1, &modes, &mode)) && modes) S.blend = mode;
    XrViewConfigurationView recommended[2] = {{XR_TYPE_VIEW_CONFIGURATION_VIEW}, {XR_TYPE_VIEW_CONFIGURATION_VIEW}};
    uint32_t views = 0;
    if (XR_SUCCEEDED(xrEnumerateViewConfigurationViews(S.instance, S.system, XR_VIEW_CONFIGURATION_TYPE_PRIMARY_STEREO, 2, &views, recommended)) && views == 2) {
        S.eyeWidth = recommended[0].recommendedImageRectWidth;
        S.eyeHeight = recommended[0].recommendedImageRectHeight;
        LOG("[vr] an eye's picture: %ux%u recommended", S.eyeWidth, S.eyeHeight);
    }
    if (!make_actions() || !attach_actions()) LOG("[vr] the headset's controllers are not available; a gamepad or the keyboard still plays");
    if (S.refreshRates) {
        // WWHD_VR_HZ=120 asks the headset for that refresh rate (frame interpolation draws 60 or 120 fps:
        // at 120 Hz every frame is shown for the same time)
        uint32_t count = 0;
        S.enumerateRefreshRates(S.session, 0, &count, nullptr);
        std::vector<float> rates(count);
        float current = 0;
        if (count && XR_SUCCEEDED(S.enumerateRefreshRates(S.session, count, &count, rates.data())) && XR_SUCCEEDED(S.getRefreshRate(S.session, &current))) {
            std::string list;
            for (float hz : rates) list += (list.empty() ? "" : ", ") + std::to_string((int)std::lround(hz));
            LOG("[vr] refresh rate %.0f Hz (the headset offers %s)", current, list.c_str());
            if (const char* e = getenv("WWHD_VR_HZ")) {
                const float want = (float)atof(e);
                const auto it = std::find_if(rates.begin(), rates.end(), [&](float hz) { return std::fabs(hz - want) < 0.5f; });
                if (it == rates.end()) LOG("[vr] WWHD_VR_HZ=%s: the headset does not offer that rate", e);
                else if (ok(S.requestRefreshRate(S.session, *it), "xrRequestDisplayRefreshRateFB")) LOG("[vr] asked for %.0f Hz", *it);
            }
        }
    }
    g_active = true;
    LOG("[vr] session created: the game's picture goes to the headset once it is ready");
    return true;
}

// ---------------------------------------------------------------- frame (render thread)
bool layer_wanted(int layer) { return layer == kScreen || gamepad_shown(); }
int failures() { return g_failures.load(); }

bool frame_open() { return S.frameOpen; }
bool frame_begin() {
    if (!S.session) return false;
    if (S.frameOpen) frame_end();  // (a world-mode frame whose second eye never came)
    poll_events();
    if (!S.session || !g_running.load()) return false;
    for (Chain& c : S.chains)
        if (c.held) {  // a frame that did not finish (an error while drawing)
            XrSwapchainImageReleaseInfo info{XR_TYPE_SWAPCHAIN_IMAGE_RELEASE_INFO};
            xrReleaseSwapchainImage(c.handle, &info);
            c.held = false;
        }
    XrFrameState frame{XR_TYPE_FRAME_STATE};
    Clock::time_point named;
    XrResult r = clock_take(frame, named);
    if (r == XR_ERROR_SESSION_LOST || r == XR_ERROR_INSTANCE_LOST) {
        leave("the headset's session was lost");
        return false;
    }
    if (!ok(r, "xrWaitFrame") || r != XR_SUCCESS) return false;
    XrFrameBeginInfo begin{XR_TYPE_FRAME_BEGIN_INFO};
    if (!ok(xrBeginFrame(S.session, &begin), "xrBeginFrame")) return false;
    clock_request();  // the wait for the frame after this one runs while this one is drawn
    S.frame = frame;
    S.frameOpen = true;
    S.serial++;
    g_displayTime.store(frame.predictedDisplayTime, std::memory_order_relaxed);
    g_displayAt.store(named.time_since_epoch().count(), std::memory_order_relaxed);
    g_period.store(frame.predictedDisplayPeriod, std::memory_order_relaxed);
    if (frame.predictedDisplayPeriod > 0) g_hz = (int)std::lround(1e9 / (double)frame.predictedDisplayPeriod);
    if (!frame.shouldRender) {  // not shown now: the frame still has to end
        frame_end();
        return false;
    }
    if (g_recentre.exchange(false)) S.anchored = false;
    if (!S.anchored) set_anchor();
    return true;
}

bool frame_acquire(int layer, Target& target) {
    if (!S.frameOpen || layer < 0 || layer >= kLayers) return false;
    Chain& c = S.chains[layer];
    // the screen: 1920x1080 holds what a headset of today can show of a screen this wide; the GamePad
    // picture: its own size; an eye: what the runtime recommends
    const bool eye = layer == kEyeLeft || layer == kEyeRight;
    const uint32_t width = eye ? (S.eyeWidth ? S.eyeWidth : 1664) : layer == kScreen ? 1920 : 854;
    const uint32_t height = eye ? (S.eyeHeight ? S.eyeHeight : 1760) : layer == kScreen ? 1080 : 480;
    if (!c.handle && !make_chain(c, width, height)) return false;
    XrSwapchainImageAcquireInfo acquire{XR_TYPE_SWAPCHAIN_IMAGE_ACQUIRE_INFO};
    if (!ok(xrAcquireSwapchainImage(c.handle, &acquire, &c.index), "xrAcquireSwapchainImage")) return false;
    c.held = true;
    XrSwapchainImageWaitInfo wait{XR_TYPE_SWAPCHAIN_IMAGE_WAIT_INFO};
    wait.timeout = XR_INFINITE_DURATION;
    if (!ok(xrWaitSwapchainImage(c.handle, &wait), "xrWaitSwapchainImage")) {
        frame_release(layer);
        return false;
    }
    target.image = c.images[c.index].image;
    target.format = (VkFormat)S.format;
    target.width = c.width;
    target.height = c.height;
    return true;
}
void frame_picture(int layer, uint32_t width, uint32_t height) {
    if (layer < 0 || layer >= kLayers) return;
    S.chains[layer].pw = std::min(width, S.chains[layer].width);
    S.chains[layer].ph = std::min(height, S.chains[layer].height);
}
void frame_release(int layer) {
    if (layer < 0 || layer >= kLayers) return;
    Chain& c = S.chains[layer];
    if (!c.held) return;
    c.held = false;
    XrSwapchainImageReleaseInfo info{XR_TYPE_SWAPCHAIN_IMAGE_RELEASE_INFO};
    if (ok(xrReleaseSwapchainImage(c.handle, &info), "xrReleaseSwapchainImage") && c.pw && c.ph) {
        c.ready = true;
        c.drawn = S.serial;
    }
}
void frame_eye(int eye, const EyeView& view) {
    if (eye == 0 || eye == 1) S.eyes[eye] = view;
}

void frame_end() {
    if (!S.frameOpen) return;
    S.frameOpen = false;
    for (int i = 0; i < kLayers; i++) frame_release(i);
    const Options o = options();
    XrCompositionLayerQuad quads[kLayers];
    XrCompositionLayerCylinderKHR cylinder{XR_TYPE_COMPOSITION_LAYER_CYLINDER_KHR};
    XrCompositionLayerProjection world{XR_TYPE_COMPOSITION_LAYER_PROJECTION};
    XrCompositionLayerProjectionView eyes[2] = {{XR_TYPE_COMPOSITION_LAYER_PROJECTION_VIEW}, {XR_TYPE_COMPOSITION_LAYER_PROJECTION_VIEW}};
    const XrCompositionLayerBaseHeader* layers[kLayers];
    uint32_t count = 0;
    // world mode: both eyes were drawn in this frame. They are the whole view, with the poses their
    // pictures were drawn with (the compositor turns them to where the head is when they are shown);
    // the screen stays away then.
    // A picture is drawn with the same field of view to both sides (world.cpp), wide enough for
    // either eye; an eye's own is lopsided (a Quest 2's left eye sees 52 degrees to the left and 45
    // to the right). What is handed over is the part of the picture the eye sees, with exactly that
    // part's field of view. Handed over whole, with the wider field of view named in the layer, the
    // two eyes' pictures stood many degrees apart in a Quest 2 (Meta's runtime 1.208) and did not
    // merge: it seems to show a picture as if it had the eye's own field of view.
    const bool worldFrame = S.frame.shouldRender && S.chains[kEyeLeft].drawn == S.serial && S.chains[kEyeRight].drawn == S.serial;
    if (worldFrame) {
        XrViewLocateInfo info{XR_TYPE_VIEW_LOCATE_INFO};
        info.viewConfigurationType = XR_VIEW_CONFIGURATION_TYPE_PRIMARY_STEREO;
        info.displayTime = S.frame.predictedDisplayTime;
        info.space = S.local;
        XrViewState state{XR_TYPE_VIEW_STATE};
        XrView own[2] = {{XR_TYPE_VIEW}, {XR_TYPE_VIEW}};
        uint32_t views = 0;
        const bool located = XR_SUCCEEDED(xrLocateViews(S.session, &info, &state, 2, &views, own)) && views == 2;
        for (int e = 0; e < 2; e++) {
            const Chain& c = S.chains[kEyeLeft + e];
            const EyeView& v = S.eyes[e];
            eyes[e].pose = {{v.q[0], v.q[1], v.q[2], v.q[3]}, {v.p[0], v.p[1], v.p[2]}};
            eyes[e].subImage.swapchain = c.handle;
            // tangents: the picture's edges, and the eye's own between them
            const float l = std::tan(v.fov[0]), r = std::tan(v.fov[1]), u = std::tan(v.fov[2]), d = std::tan(v.fov[3]);
            float x0 = 0, x1 = 1, y0 = 0, y1 = 1;  // parts of the picture, from its top left
            if (located && r - l > 0.1f && u - d > 0.1f) {
                const XrFovf& f = own[e].fov;
                x0 = std::clamp((std::tan(f.angleLeft) - l) / (r - l), 0.0f, 1.0f);
                x1 = std::clamp((std::tan(f.angleRight) - l) / (r - l), 0.0f, 1.0f);
                y0 = std::clamp((u - std::tan(f.angleUp)) / (u - d), 0.0f, 1.0f);
                y1 = std::clamp((u - std::tan(f.angleDown)) / (u - d), 0.0f, 1.0f);
                if (x1 - x0 < 0.25f || y1 - y0 < 0.25f) x0 = y0 = 0, x1 = y1 = 1;
            }
            const int32_t px0 = (int32_t)std::lround(x0 * c.pw), px1 = (int32_t)std::lround(x1 * c.pw);
            const int32_t py0 = (int32_t)std::lround(y0 * c.ph), py1 = (int32_t)std::lround(y1 * c.ph);
            eyes[e].subImage.imageRect = {{px0, py0}, {px1 - px0, py1 - py0}};
            eyes[e].fov = {std::atan(l + (r - l) * (float)px0 / (float)c.pw), std::atan(l + (r - l) * (float)px1 / (float)c.pw),
                           std::atan(u - (u - d) * (float)py0 / (float)c.ph), std::atan(u - (u - d) * (float)py1 / (float)c.ph)};
        }
        world.space = S.local;
        world.viewCount = 2;
        world.views = eyes;
        layers[count++] = reinterpret_cast<const XrCompositionLayerBaseHeader*>(&world);
    }
    // debug: WWHD_VR_TRACE=n logs what the first n world frames were drawn with (and every 20th after),
    // beside where the eyes are now, and every 300 frames how many of them were the world's
    static const int traceN = getenv("WWHD_VR_TRACE") ? atoi(getenv("WWHD_VR_TRACE")) : 0;
    if (traceN > 0 && S.frame.shouldRender) {
        static int traced = 0, worldFrames = 0, frames = 0;
        static uint64_t seen = 0;
        worldFrames += worldFrame;
        if (++frames == 300) {
            LOG("[vr] trace: %d of the last 300 frames were the world's (both eyes)", worldFrames);
            frames = worldFrames = 0;
        }
        if (worldFrame && (traced < traceN || ++seen % 20 == 0)) {
            traced++;
            XrViewLocateInfo info{XR_TYPE_VIEW_LOCATE_INFO};
            info.viewConfigurationType = XR_VIEW_CONFIGURATION_TYPE_PRIMARY_STEREO;
            info.displayTime = S.frame.predictedDisplayTime;
            info.space = S.local;
            XrViewState state{XR_TYPE_VIEW_STATE};
            XrView now[2] = {{XR_TYPE_VIEW}, {XR_TYPE_VIEW}};
            uint32_t n = 0;
            xrLocateViews(S.session, &info, &state, 2, &n, now);
            for (int e = 0; e < 2; e++) {
                const XrCompositionLayerProjectionView& v = eyes[e];
                const layout::Vec3 ahead = layout::rotate(from(v.pose.orientation), {0, 0, -1}), up = layout::rotate(from(v.pose.orientation), {0, 1, 0});
                const layout::Vec3 aheadNow = layout::rotate(from(now[e].pose.orientation), {0, 0, -1});
                LOG("[vr] trace frame %llu %s: drawn at (%.3f %.3f %.3f) ahead (%.3f %.3f %.3f) up (%.3f %.3f %.3f) fov %.1f %.1f %.1f %.1f, image %dx%d from (%d %d); "
                    "now at (%.3f %.3f %.3f) ahead (%.3f %.3f %.3f) fov %.1f %.1f %.1f %.1f",
                    (unsigned long long)S.serial, e ? "right" : "left ", v.pose.position.x, v.pose.position.y, v.pose.position.z, ahead.x, ahead.y, ahead.z, up.x, up.y,
                    up.z, v.fov.angleLeft * 180 / kPi, v.fov.angleRight * 180 / kPi, v.fov.angleUp * 180 / kPi, v.fov.angleDown * 180 / kPi,
                    v.subImage.imageRect.extent.width, v.subImage.imageRect.extent.height, v.subImage.imageRect.offset.x, v.subImage.imageRect.offset.y,
                    now[e].pose.position.x, now[e].pose.position.y, now[e].pose.position.z,
                    aheadNow.x, aheadNow.y, aheadNow.z, now[e].fov.angleLeft * 180 / kPi, now[e].fov.angleRight * 180 / kPi, now[e].fov.angleUp * 180 / kPi,
                    now[e].fov.angleDown * 180 / kPi);
            }
            // the right eye as the left eye sees it: to its right (+x) by the distance between the eyes
            const layout::Vec3 across = layout::rotate(layout::conj(from(eyes[0].pose.orientation)), from(eyes[1].pose.position) - from(eyes[0].pose.position));
            LOG("[vr] trace frame %llu: the right eye from the left one: %.3f %.3f %.3f m", (unsigned long long)S.serial, across.x, across.y, across.z);
        }
    }
    layout::Placement screen;
    if (S.frame.shouldRender)
        for (int i = 0; i <= kGamePad; i++) {
            const Chain& c = S.chains[i];
            // (the panel's picture is this frame's: an older one is from before it was put away)
            if (!c.ready || !layer_wanted(i) || (worldFrame && i == kScreen) || (i == kGamePad && c.drawn != S.serial)) continue;
            const float aspect = (float)c.pw / (float)c.ph;
            XrSwapchainSubImage image{};
            image.swapchain = c.handle;
            image.imageRect.extent = {(int32_t)c.pw, (int32_t)c.ph};
            if (i == kGamePad) {
                // In the player's left hand: the layer is given in the controller's own space, so the
                // compositor moves it with the hand at every refresh. Not shown while the controller
                // is not tracked.
                const XrSpace hand = S.actions.aimSpace[kLeft];
                XrSpaceLocation where{XR_TYPE_SPACE_LOCATION};
                const XrSpaceLocationFlags need = XR_SPACE_LOCATION_ORIENTATION_VALID_BIT | XR_SPACE_LOCATION_POSITION_VALID_BIT;
                if (!hand || XR_FAILED(xrLocateSpace(hand, S.local, S.frame.predictedDisplayTime, &where)) || (where.locationFlags & need) != need) continue;
                g_padAspect.store(aspect, std::memory_order_relaxed);
                XrCompositionLayerQuad& q = quads[i];
                q = {XR_TYPE_COMPOSITION_LAYER_QUAD};
                q.space = hand;
                q.eyeVisibility = XR_EYE_VISIBILITY_BOTH;
                q.subImage = image;
                q.pose = to_xr(layout::panel_in_hand());
                q.size = {layout::kPanelWidth, layout::kPanelWidth / aspect};
                layers[count++] = reinterpret_cast<const XrCompositionLayerBaseHeader*>(&q);
                continue;
            }
            const layout::Placement p = screen = layout::place_screen(S.anchor, o.distance, o.size, o.curve, o.height, aspect, S.cylinder);
            if (p.cylinder) {
                cylinder.space = S.local;
                cylinder.eyeVisibility = XR_EYE_VISIBILITY_BOTH;
                cylinder.subImage = image;
                cylinder.pose = to_xr(p.pose);
                cylinder.radius = p.radius;
                cylinder.centralAngle = p.angle;
                cylinder.aspectRatio = aspect;
                layers[count++] = reinterpret_cast<const XrCompositionLayerBaseHeader*>(&cylinder);
            } else {
                XrCompositionLayerQuad& q = quads[i];
                q = {XR_TYPE_COMPOSITION_LAYER_QUAD};
                q.space = S.local;
                q.eyeVisibility = XR_EYE_VISIBILITY_BOTH;
                q.subImage = image;
                q.pose = to_xr(p.pose);
                q.size = {p.width, p.height};
                layers[count++] = reinterpret_cast<const XrCompositionLayerBaseHeader*>(&q);
            }
        }
    XrFrameEndInfo end{XR_TYPE_FRAME_END_INFO};
    end.displayTime = S.frame.predictedDisplayTime;
    end.environmentBlendMode = S.blend;
    end.layerCount = count;
    end.layers = count ? layers : nullptr;
    const XrResult r = xrEndFrame(S.session, &end);
    if (r == XR_ERROR_SESSION_LOST || r == XR_ERROR_INSTANCE_LOST) leave("the headset's session was lost");
    else if (!ok(r, "xrEndFrame") && screen.cylinder) {
        LOG("[vr] the headset refused the curved screen: flat from now on");
        S.cylinder = false;
    }
}

// ---------------------------------------------------------------- controllers (main thread)
bool poll_controllers(float* values) {
    std::lock_guard lk(S.mu);
    if (!S.session || !g_running.load() || !S.actions.set) return false;
    const auto now = Clock::now();
    if (now - S.polled >= std::chrono::milliseconds(4)) {
        S.polled = now;
        read_controllers(now);
    }
    for (int p = 0; p < input_map::kPadCount; p++) values[p] = std::max(values[p], S.pad[p]);
    return S.padActive;
}
// The eyes at the time a picture drawn now is shown: three refreshes after the time the headset
// would show a frame begun now (the game draws the picture, the renderer draws it a pass later, the
// other eye's follows, the headset shows them after that). The headset named a time when it last
// asked for a frame; what has passed since then is added. The compositor corrects the rest from the
// pose given back with the picture.
bool locate_eyes(EyeView eyes[2], float anchor[4]) {
    std::lock_guard lk(S.mu);
    if (!S.session || !g_running.load() || !S.anchored) return false;
    const XrTime shown = g_displayTime.load(std::memory_order_relaxed);
    if (!shown) return false;
    XrViewLocateInfo info{XR_TYPE_VIEW_LOCATE_INFO};
    info.viewConfigurationType = XR_VIEW_CONFIGURATION_TYPE_PRIMARY_STEREO;
    const int64_t since = std::chrono::duration_cast<std::chrono::nanoseconds>(
                              Clock::now() - Clock::time_point(Clock::duration(g_displayAt.load(std::memory_order_relaxed)))).count();
    info.displayTime = shown + std::clamp<int64_t>(since, 0, 200'000'000) + 3 * g_period.load(std::memory_order_relaxed);
    info.space = S.local;
    XrViewState state{XR_TYPE_VIEW_STATE};
    XrView views[2] = {{XR_TYPE_VIEW}, {XR_TYPE_VIEW}};
    uint32_t n = 0;
    if (XR_FAILED(xrLocateViews(S.session, &info, &state, 2, &n, views)) || n != 2) return false;
    const XrViewStateFlags need = XR_VIEW_STATE_ORIENTATION_VALID_BIT | XR_VIEW_STATE_POSITION_VALID_BIT;
    if ((state.viewStateFlags & need) != need) return false;
    for (int e = 0; e < 2; e++) {
        const XrView& v = views[e];
        eyes[e] = {{v.pose.orientation.x, v.pose.orientation.y, v.pose.orientation.z, v.pose.orientation.w},
                   {v.pose.position.x, v.pose.position.y, v.pose.position.z},
                   {v.fov.angleLeft, v.fov.angleRight, v.fov.angleUp, v.fov.angleDown}};
    }
    anchor[0] = S.anchor.position.x;
    anchor[1] = S.anchor.position.y;
    anchor[2] = S.anchor.position.z;
    anchor[3] = S.anchor.yaw;
    return true;
}
void show_gamepad(bool on) { g_padOut = on; }
bool gamepad_shown() { return g_padOut.load(std::memory_order_relaxed) && options().gamepad; }
bool gamepad_pointer(float* u, float* v, bool* pressed) {
    std::lock_guard lk(S.mu);
    if (!S.pointing) return false;
    *u = S.pointerU;
    *v = S.pointerV;
    *pressed = S.pointerPressed;
    return true;
}

}  // namespace xr
