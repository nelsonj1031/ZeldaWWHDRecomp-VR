# VR headsets (PC VR through OpenXR)

The game inside a VR headset, played with the headset's controllers. The game runs on the computer;
the headset is its display and its controllers. Any headset with an OpenXR runtime on the PC should
work; it was developed with a **Meta Quest 2 over Quest Link**.

There are two ways to see the game:

- **[World mode](#world-mode-the-game-world-around-you-in-3d)** (the default): the game's world
  around you in stereoscopic 3D. You stand where the game's camera is and look around with your own
  head.
- **Screen**: the picture the TV window shows, on a large screen that stands still in front of you.
  Menus without a 3D scene show on it in world mode too.

In both, a click of the left stick takes the **GamePad screen** out on your left controller.

[What was tested](#what-was-tested) says how far each of these was checked in a headset.

## Why PC VR, and no app that runs on the Quest 2 itself

The Android port cannot run on a Quest 2. Read from a Quest 2 on Horizon OS (Android 14, build 207)
with `adb shell cmd gpu vkjson`:

- its GPU driver is **Vulkan 1.1** (Adreno 650, driver 512.819) and has no `VK_KHR_dynamic_rendering`;
  the renderer needs Vulkan 1.3, or 1.1 / 1.2 with that extension (`gfx/vulkan/backend.cpp`), for all
  of its drawing;
- the port holds 30 fps in heavy scenes on a Snapdragon 8 Elite phone with the render thread at
  30 to 40 ms a frame (README, Android). The Quest 2's chip is several generations older; expect
  roughly a third of that speed (an estimate from the chips, not a measurement).

A standalone app would need the renderer ported to render passes and framebuffers first, and would
still be far too slow. Over Link the PC does the work and the limits are the PC's.

## Requirements

- Windows, the Vulkan renderer (the default there). Linux builds take `-DWWHD_OPENXR=ON` but are untested.
- An OpenXR runtime that accepts Vulkan applications, set as the active runtime. For a Quest: the Meta
  Quest Link app (*Settings > General > OpenXR Runtime*), with Link or Air Link started in the headset
  **before** the game starts. SteamVR and Virtual Desktop provide runtimes too (untested here).
- Controllers with the Touch layout (Quest, Rift) for the mapping below. A gamepad or the keyboard
  plays as well; gamepads keep working while the game's window is not in front.

## Building

VR support is the CMake option `WWHD_OPENXR`: on by default on Windows, off elsewhere. The Khronos
OpenXR loader is found as an installed package or built from its pinned source release
(`cmake/OpenXR.cmake`) and linked statically on Windows, so the game starts the same on a computer
without any VR software. `-DWWHD_OPENXR=OFF` builds without it.

## Playing

Start with `--vr`, or turn on **Settings overlay > Display > VR headset > Play in the VR headset**
(it counts from the next start; `--no-vr` and `WWHD_VR=0|1` decide one start). Without a headset the
game says why in its log and in that settings section, and plays in its windows as usual.

In the headset:

- **The world** is around you as soon as the game draws a 3D scene; see
  [World mode](#world-mode-the-game-world-around-you-in-3d).
- **The screen** (menus without a 3D scene, the settings, or the whole game with world mode off)
  stands where you looked when the first frame was drawn. *Recentre the screen* (or hold the left
  stick down and click the right one) moves it in front of you again; so does the headset's own
  recentring. Size (degrees of your view), distance, height and a curve are in the settings section
  and apply at once. The curve needs a runtime with cylinder layers (Meta's has them).
- **The GamePad screen** comes out on your left controller when you click the left stick (press it
  and let go), like a tablet standing on the controller, and goes away with the next click. Point
  the right controller at it: a dot shows where, and the right trigger touches the screen there
  (hold it to drag) instead of being ZR. It can be turned off in the settings section. The other
  GamePad screen modes (picture-in-picture, GamePad only) work inside the screen as they do in the
  TV window.
- **The settings overlay** shows on the screen; the controllers move in it (A confirms, B goes back).
- The TV window on the monitor mirrors the picture without waiting for the monitor's vsync.
- Taking the headset off ends its session; the game goes on in its windows and returns to the headset
  when you put it on again. If the headset's software closes the session for good (Link ends), start
  the game again to get back in.

### Controllers

The headset's controllers are one more host controller: their inputs go through the controls mapping
(*Controls* tab), like a gamepad's. With the default controls:

| Touch controller | Wii U GamePad |
|---|---|
| left / right stick | move / camera |
| A, B, X, Y | A, B, X, Y (by their letters) |
| left / right trigger | ZL (target) / ZR |
| left / right grip | L / R |
| right stick click | right stick click (first person) |
| Menu (left) | Plus; **held for half a second: the settings overlay** |
| **left stick clicked** (down and up at once) | the GamePad screen on the left controller, or away again |
| right controller pointed at the GamePad screen | pointer; **trigger touches** the GamePad screen |
| **left stick held down** ("shift") + right stick | D-pad |
| shift + Menu | Minus |
| shift + right stick click | recentre |

The left stick's own click is not passed on to the game. The system button (Meta / Oculus) stays the
headset's.

**Aiming with the right controller:** *Controls > Gyro… > VR controller (right hand)*. The game aims
the bow, hookshot, boomerang, telescope, Picto Box and grappling hook by how the GamePad turns; with
this source it turns as the right controller's aim does (left/right about the real vertical, up/down).
Sensitivity and invert are the gyro settings (docs/gyro.md); the game's own *Options > Gyro* switch
still applies.

**Rumble** plays on both controllers (*Rumble on the VR controllers* in the settings section).

### World mode: the game world around you in 3D

On unless turned off: *Display > VR headset > The game world around you, in 3D (world mode)*; it
applies at once and is saved. `WWHD_VR_WORLD=0|1` decides one start.

- **You are the game's camera.** The game stays a third-person game: you float behind Link where
  its camera is, the left stick moves Link, the right stick turns the camera and you with it. Your
  head's turn and movement go on top: look around freely, lean, stand up.
- **The horizon stays level.** Only the camera's position and heading are taken from the game; its
  own tilt up and down is left out, because a view that tilts without your head doing so is what
  makes people sick. While Link aims in first person (bow, hookshot, boomerang, grappling hook) the
  game's whole aiming direction counts, so looking straight ahead looks where the game aims.
- **Recentring** (shift + right stick click, or the headset's own) makes the way you face now the
  game camera's direction.
- **World scale** (game units to the metre) sets how large the world is around you and how far a
  step of yours moves the view; 100 is about life size.
- **The HUD** (hearts, buttons, rupees) is on a panel that floats two metres ahead of you in the
  room, level with the horizon; **HUD size** is how much of your view it takes. It stands still
  while you move your head a little, glides after your gaze when you look further away, and is
  never more than about 30 degrees behind it.
- **What still shows on the screen:** scenes without a game camera (file select, name entry), the
  settings overlay, and zoomed views, the telescope and the Picto Box (a camera narrower than 30
  degrees: an eye cannot zoom).
- **Sharpness:** an eye's picture is square, as wide as the internal resolution (*Graphics >
  Internal resolution*): 1280 pixels at 1x for about 100 degrees is soft in a headset; 2x (2560)
  is about what a Quest 2 shows, and costs graphics card time, not the processor time that limits
  world mode.

World mode draws the game twice for every picture, once per eye, as two passes of
[frame interpolation](../README.md): it switches frame interpolation on at 120 fps, which gives 60
pictures a second in the headset, and each game step gets as many pairs of pictures as the computer
draws in time, never less than one (30 pictures a second). The game keeps its speed either way. The
frame rate setting stays at 120 fps interpolation when world mode is turned off again.

Known limits:

- **It is heavy.** Every pass is a whole frame of the game on its one render thread. On the
  computer it was written on, Outset Island (about 5000 draws a frame) comes out at one pair per
  game step: 27 to 29 pictures a second in a Quest 2, with the game at 91 to 98 % of its speed
  where a pair takes longer than a game step. The headset's compositor still turns the last picture
  with your head, so looking around is steady; what moves in the game moves at that rate.
- The title screen's boat and logo, and a few small 2D elements that are not part of the game's
  layout system, are drawn flat at their place in each eye's picture and do not line up in depth.
- Expected from how it works and from other VR ports, not checked: cutscenes move you with their
  camera, cuts included; effects that work on the finished picture (heat haze, depth of field, the
  edges of fades) are computed per eye and can differ between the eyes.

### Frame rate and the headset's refresh rate

The headset's compositor draws the screen at its own refresh rate with your head's latest position,
so the screen itself is steady at any game frame rate. What the picture on it does depends on how the
game's frames fall on the headset's refreshes:

| Headset | Even cadence with |
|---|---|
| 72 Hz | nothing the game draws (30 fps is 2.4 refreshes a frame) |
| 90 Hz | 30 fps (3 refreshes a frame) |
| 120 Hz | 30 fps, and frame interpolation at 60 or 120 fps |

For a Quest over Link the refresh rate is chosen in the Meta Quest Link app (*Devices > the headset >
Graphics Preferences*); 120 Hz with frame interpolation at 60 or 120 fps is the smoothest. Frame
interpolation is capped to the headset's rate, not the monitor's, while the headset runs.
`WWHD_VR_HZ=90` asks the runtime for a rate at start (runtimes with `XR_FB_display_refresh_rate`;
Link offers only the rate chosen in its app).

World mode's 60 or 30 pictures a second fall evenly on 120 Hz only (2 or 4 refreshes a picture), and
30 on 90 Hz.

### Options for one start

`WWHD_VR=0|1`, `WWHD_VR_WORLD=0|1`, `WWHD_VR_SIZE=<degrees>`, `WWHD_VR_DISTANCE=<metres>`,
`WWHD_VR_CURVE=<0..1>`, `WWHD_VR_HZ=<rate>`. They are not saved.

### Checking a setup without the game

```
wwhd --vr-smoke [seconds]
```

opens a session on the actual headset with a blue screen, the settings overlay on its Display tab and
a sand-coloured GamePad panel, logs what the controllers send (`[vr smoke] controller: ...`) and ends
with PASS, FAIL, or INCONCLUSIVE when a session exists but the headset never took a frame (it is not
worn, or Link is not started). The `[vr]` lines of the log say which runtime and headset were found
and every change of the session's state.

## How it works

- `runtime/src/xr/xr.cpp` (API in `xr.h`, `xr_vulkan.h`): the OpenXR instance and session with the
  `XR_KHR_vulkan_enable` binding, so the renderer keeps creating its own Vulkan instance and device;
  the runtime only names the extensions and the GPU it needs (`init_device` in
  `gfx/vulkan/backend.cpp`). Any step that fails leaves the game in its windows.
- The screen is a **quad layer** (a **cylinder layer** when curved) in a LOCAL reference space. The
  runtime's compositor reprojects it at every refresh, which is why the screen stands still at
  30 fps. The GamePad panel is a quad layer in the left controller's own (aim pose) space, so the
  compositor moves it with the hand at every refresh too. `xr/layout.h` places them (unit-tested:
  `xr_layout_test`).
- `gfx/vulkan/xr_present.cpp`, called from `swap()`: `headset_begin` waits for the headset's next
  frame (`xrWaitFrame`: it paces the game as a display's vsync does) and `headset_draw` runs the
  window composition of `present.cpp` (scaling filter, FXAA, picture-in-picture, settings overlay)
  into the layers' images instead of a window's.
- The controllers are OpenXR actions for `/interaction_profiles/oculus/touch_controller`, read on the
  host's main loop (`poll_controllers`, from `platform/input_sdl.cpp`) into the same values a
  gamepad fills.
- At an orderly exit the session and instance are closed before the process ends
  (`save_renderer_caches`); Meta's runtime aborts inside its own teardown otherwise.

World mode (`xr/world.h`, `world.cpp`):

- **Two eyes are two passes of one game step.** Frame interpolation (`interp.cpp`) already draws
  the game several times per logic step without running its logic. World mode draws these passes
  in pairs, left eye then right, both at the same blend fraction (`pair_phase`), so both eyes show
  the same moment. A step is planned with a whole number of pairs (`plan_step`); a pass is never
  skipped inside a pair.
- **The camera.** The hook on `camera_draw` (`024FFC40`) hands the camera it is about to draw to
  `xrworld::eye_camera`, which asks the runtime where the eyes will be when the picture is shown
  (`xr::locate_eyes`) and rewrites eye, centre, up, fovy and bank (`eye_camera` in `xr/layout.h`,
  unit-tested). The VR ports of comparable games do the same: both eyes from one game state, only
  the heading from the game's camera (BetterVR for Breath of the Wild, Dolphin VR, TPVR and
  dusklight-vr for Twilight Princess).
- **Projection, culling, render targets** follow through the aspect ratio code (`aspect.cpp`): an
  eye's picture has one shape and a symmetric field of view wide enough for either eye as the
  runtime reports them, and the game's clipper is set up for it, so nothing vanishes at the edge.
- **To the headset.** The eye's pose and field of view travel to the render thread in the GX2
  command stream (`OP_VR_EYE`). The game paints a pass's draw lists during the next pass, so the
  picture that belongs to a camera ends two swaps later; `world.cpp` keeps that delay. At the swap
  `headset_eye` copies the picture into that eye's swapchain image; after the right eye the frame
  is ended with one **projection layer**, with the pose the pictures were drawn for, so the
  compositor can correct for where the head is by then.
- **The headset's frame clock.** `xrWaitFrame` blocks until the headset wants the next frame begun,
  up to a refresh period. On the render thread that wait came on top of the drawing, and a pair of
  pictures drawn in 35 ms was shown every 41.7 ms (three refreshes at 72 Hz: 24 a second, the game
  at 81 % of its speed). A thread of its own now waits for the next frame while this one is drawn
  (`FrameClock` in `xr.cpp`), and the render thread waits only when it is ahead of the headset.
- **An eye's own field of view.** A headset's eyes see further to the outside than to the nose (a
  Quest 2's left eye: 52 degrees to the left, 45 to the right), and the game can only draw a view
  that is the same to both sides. So a picture is drawn wide enough for either eye, and
  `frame_end` hands over the part of it that the eye sees (`subImage.imageRect`) with that part's
  field of view. Handed over whole, with the wider field of view named in the layer, the two
  eyes' pictures stood many degrees apart in a Quest 2 and did not merge.
- **The HUD.** A HUD drawn at a fixed place of the eyes' pictures stutters whenever the head turns:
  the compositor turns each picture with the room until the next one comes, the HUD in it too.
  So the HUD is a panel in the room. The projections of the game's 2D layouts pass through
  `OP_SET_PROJ_REGS`; while an eye's picture is drawn, `hud_projection` puts a matrix in front of
  them that takes where the layout would be in the view to where that panel is in this eye's
  picture (`hud_matrix`), which also gives each eye its own view of it. Once per frame the panel
  follows the head's direction (`follow_hud`): not within a few degrees, beyond that with a time
  constant of a quarter of a second, and on a leash.

A head without a headset, for work on world mode: `WWHD_VR_TEST_HEAD="yaw,pitch,x,y,z[,eye distance]"`
(degrees to the left and up, metres right, up and back; the eyes 0.064 m apart unless given) runs
world mode with that head and no session (a seventh number tilts the head). The two eyes'
pictures alternate in the TV window and in `WWHD_DUMP_FRAMES`. `WWHD_VR_TEST_HUD="yaw,pitch"` keeps
the HUD's panel in that direction instead of following.

What the headset really shows, for both eyes: Meta's `OculusMirror.exe` (in the Link app's
`Support\oculus-diagnostics`) with `--RectilinearBothEyes`, while the headset is worn. With
`--DisableTimewarp` it shows the pictures as handed over. `WWHD_VR_TRACE=n` logs the pose, field
of view and image part of the first n world frames and of every 20th after them.

## What was tested

Hardware: a Quest 2 over Link (Meta runtime 1.208, 72 Hz), RTX 3090, Windows 10.

In the headset:

- with `--vr-smoke` (placeholder game code): the session starts and reaches *focused*, frames are
  taken at the headset's rate (71.9 a second at 72 Hz) with no failing OpenXR call, the Touch
  controllers' buttons, sticks and grips arrive, and the process ends cleanly;
- the game itself on the screen (world mode off) for two minutes: shown, controllers found, no
  failing OpenXR call, clean exit;
- the game in world mode, by the player's report and from mirror captures: the two eyes' views
  merge and the 3D world looks right (after the fix for an eye's own field of view; before it
  they did not merge), the GamePad screen comes out on the left controller, and on Outset Island
  the log shows 27 to 29 pairs of pictures a second (24 before the frame clock had its own thread);
- without the headset worn, and without Link, the game starts and plays in its windows.

World mode, with the game on Outset Island and the test head above, from frame dumps and logs; no
headset was involved:

- an eye's picture has the field of view asked for (the test head's is 100 by 100 degrees; 50.2
  degrees to each side measured);
- left and right pictures show the same moment (with no distance between the eyes they are equal
  pixel for pixel) and near things shift the right way between them, the HUD too;
- turning the test head to the sides, up, down and behind shows the world with nothing culled;
- the game keeps its speed (29.6 logic steps a second with two passes per step).

Not yet checked with eyes in a headset: the HUD's panel while the head moves (seen so far only in
frame dumps with the test head: in its place, level, in front of the world), whether scale feels
right, comfort over a longer time, cutscenes, aiming, the pointer and touching on the GamePad
screen, the curved screen, haptics, and the gyro source while aiming.

## What is next

- A lighter second eye. The second pass issues the same draws again; drawing both eyes from one
  pass (the same draw lists with two view matrices, or multiview) should cost much less.
- The HUD on a layer of its own, which the compositor would draw at the headset's rate (it is in
  the eyes' pictures, so it moves at the game's rate when its panel follows the head).
- A first-person view (the camera at Link's head), and the telescope and Picto Box inside the world.
- Hands: the controllers' poses are read already for aiming and the pointer, but nothing in the
  world follows them.
