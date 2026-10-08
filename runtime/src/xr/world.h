// World mode of the VR headset (xr.h, docs/vr.md): the game world around the player in 3D, seen from
// the game camera's place with the player's own head on top, instead of a picture on a screen.
//
// How a frame comes about:
//  - Two eyes need two pictures of the same moment. Frame interpolation (interp.cpp) already draws
//    the game several times per logic step without running its logic; world mode keeps it on and
//    draws its passes in pairs: passes 0 and 1 of a step are the left and the right eye of one
//    frame, 2 and 3 of the next, and both passes of a pair blend at the same fraction (pair_phase).
//    Interpolation runs at 120 fps or more (two pairs per step: 60 pictures a second in the headset),
//    and each step is planned with as many whole pairs as the computer draws in time, at least one
//    (30 pictures a second), so the game keeps its speed.
//  - camera_draw (interp.cpp's hook) hands the camera it is about to draw to eye_camera, which
//    returns the eye's: the game camera's position and heading (its pitch only while Link aims),
//    plus the eye's place and direction relative to where the player sat when the screen was
//    placed, `scale` game units to the metre. The projection's field of view and shape, the
//    culling frustum and the render targets follow through aspect.cpp (aspect()).
//  - The eye's pose and field of view travel to the render thread in the command stream (a GX2
//    host command, OP_VR_EYE); the picture drawn with that camera is the one that ends two swaps
//    later (the game paints a pass's draw lists in the next pass). There the HUD's projections are
//    replaced by those of a panel that floats two metres ahead in the room and follows where the
//    head looks, late and smoothly (hud_projection; world.cpp says why), and at the swap the
//    finished picture goes into that eye's layer of the headset (gfx/vulkan/xr_present.cpp).
// Scenes without a game camera (file select, name entry) and the settings overlay show on the screen
// as before.
//
// Test aid: WWHD_VR_TEST_HEAD="yaw,pitch,x,y,z[,eye distance]" (degrees to the left / up, metres
// right / up / back; the eyes 0.064 m apart unless given; a seventh number tilts the head, degrees
// to the left) stands in for a headset: world mode with that head, no session needed. The pictures
// of the two eyes alternate in the TV window and in frame dumps.
#pragma once
#include <cstdint>

#include "xr.h"

namespace xrworld {

// ---- game thread
bool wanted();   // the option is on and there is a head to follow (also switches frame interpolation on)
bool active();   // eye pictures were drawn lately
// camera_draw: phase = the pass of the logic step (0: the logic pass), aiming = Link aims in first
// person. In: the game's camera as it would be drawn; out: this pass's eye. False: not in world mode.
bool eye_camera(int phase, bool aiming, float eye[3], float center[3], float up[3], float* fovyDegrees, int16_t* bank);
int pair_phase(int phase);  // the phase whose blend fraction both passes of a pair use (phase when not active)
float aspect();             // the game's aspect ratio while active, 0 otherwise (aspect.cpp)
float hud_scale();          // the HUD's size while active, 1 otherwise (aspect.cpp: what must still fill the view)

// ---- render thread
void eye_op(const uint32_t* words, uint32_t n);  // OP_VR_EYE: the camera of the picture after the one being drawn
struct Frame {
    bool eye = false;   // the picture in the TV scan buffer is an eye's view of the world
    bool settled = false;  // ... drawn at world mode's shape (the first frames after a change are not)
    int index = 0;      // 0 left, 1 right
    xr::EyeView view;   // what it was drawn with
};
Frame take_frame();  // at the swap: the picture that ends here
// a TV layout's projection (row-major 4x4, OP_SET_PROJ_REGS) while an eye's picture is drawn
void hud_projection(float* matrix);

}  // namespace xrworld
