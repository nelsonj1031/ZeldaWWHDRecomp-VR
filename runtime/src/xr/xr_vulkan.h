// The graphics half of xr.h, for the Vulkan renderer (gfx/vulkan/backend.cpp, xr_present.cpp).
//
// Start-up (XR_KHR_vulkan_enable: the renderer creates the Vulkan instance and device, the OpenXR
// runtime says which extensions and which GPU it needs):
//   start()                        before the Vulkan instance: the OpenXR instance and the headset
//   vulkan_instance_extensions()   to enable on the Vulkan instance
//   vulkan_physical_device()       the GPU the headset is connected to
//   vulkan_device_extensions()     to enable on the device
//   create_session()               once the device exists
// abandon() at any step leaves the game on the windows alone.
//
// A frame (render thread):
//   if (frame_begin()) {           waits for the headset's next frame; false: nothing to draw
//       frame_acquire(layer, t)    the image to draw the layer's picture into; it arrives and must
//                                  leave in VK_IMAGE_LAYOUT_COLOR_ATTACHMENT_OPTIMAL
//       frame_picture(layer, w, h) the part of the image that holds the picture, from its top left
//       ... submit the commands that draw it ...
//       frame_release(layer)
//       frame_end()                shows the layers (also those not drawn this frame)
//   }
// World mode draws the two eye layers instead of the screen, over two calls of the renderer's swap():
// frame_begin and the left eye in one, the right eye and frame_end in the next (frame_open() says the
// frame is still waiting); frame_eye() gives the pose and field of view each picture was drawn with.
// A frame with both eyes shows them as the headset's whole view; a frame begun while another is still
// open ends that one first.
// The headset's images are gone when a session ends: the hook runs first (the renderer destroys its
// views), on the render thread.
#pragma once
#ifdef WWHD_OPENXR
#include <cstdint>
#include <string>
#include <vector>

#include <vulkan/vulkan.h>

namespace xr {

bool start();
bool started();  // start() found a headset and nothing abandoned it since
void abandon(const char* why);
std::vector<std::string> vulkan_instance_extensions();
VkPhysicalDevice vulkan_physical_device(VkInstance instance);  // null: the runtime names none
std::vector<std::string> vulkan_device_extensions();
bool create_session(VkInstance instance, VkPhysicalDevice physicalDevice, VkDevice device, uint32_t queueFamily, uint32_t queueIndex);
void set_images_release_hook(void (*hook)());

enum Layer : int { kScreen, kGamePad, kEyeLeft, kEyeRight, kLayers };
struct Target {
    VkImage image = VK_NULL_HANDLE;
    VkFormat format = VK_FORMAT_UNDEFINED;
    uint32_t width = 0, height = 0;
};
bool frame_begin();
bool frame_open();
bool frame_acquire(int layer, Target& target);
void frame_eye(int eye, const EyeView& view);
void frame_picture(int layer, uint32_t width, uint32_t height);
void frame_release(int layer);
void frame_end();
bool layer_wanted(int layer);  // the GamePad panel: while the player has it out
int failures();                // OpenXR calls that failed so far (the self-test, --vr-smoke)

}  // namespace xr
#endif
