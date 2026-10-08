// VR headset (xr/xr.h): the windows' compositions drawn into the headset's layer images. The headset
// shows them as a screen standing in front of the player and a GamePad panel on their left controller
// (xr.cpp places them), or, in world mode (xr/world.h), each eye's picture of the game world as the
// headset's whole view.
#include "present.h"
#ifdef WWHD_OPENXR
#include "backend.h"
#include "xr/xr.h"
#include "xr/xr_vulkan.h"
#include <algorithm>
#include <cmath>
#include <unordered_map>
namespace gfxvk {
namespace {
std::unordered_map<VkImage,VkImageView> views;  // of the headset's layer images
xr::Target screen;                               // this frame's screen image, held from headset_begin to headset_draw
VkExtent2D screenExtent{};
// the session's images are about to be destroyed (xr.cpp, on the render thread)
void drop_views() {
 if(views.empty())return;
 vk_check(vkDeviceWaitIdle(R.device),"headset images idle");
 for(auto& [image,view]:views)vkDestroyImageView(R.device,view,nullptr);
 views.clear();
}
VkImageView view_of(const xr::Target& target) {
 if(auto it=views.find(target.image);it!=views.end())return it->second;
 VkImageViewCreateInfo ci{VK_STRUCTURE_TYPE_IMAGE_VIEW_CREATE_INFO};ci.image=target.image;ci.viewType=VK_IMAGE_VIEW_TYPE_2D;ci.format=target.format;
 ci.subresourceRange={VK_IMAGE_ASPECT_COLOR_BIT,0,1,0,1};
 VkImageView view;vk_check(vkCreateImageView(R.device,&ci,nullptr,&view),"headset image view");
 views.emplace(target.image,view);return view;
}
// a picture of this shape as large as the layer's image takes it, from the image's top left corner
VkExtent2D fit(const Surface* picture,const xr::Target& target) {
 const float aspect=picture&&picture->extent.height?float(picture->extent.width)/float(picture->extent.height):16.0f/9.0f;
 const float scale=std::min(float(target.width)/aspect,float(target.height));
 return {std::clamp(uint32_t(std::lround(scale*aspect)),1u,target.width),std::clamp(uint32_t(std::lround(scale)),1u,target.height)};
}
// the GamePad picture on its panel (while the player has it out), with the pointer where the right
// controller points: a light square in a dark frame, orange while it touches
void draw_pad(Surface* drcScan) {
 xr::Target pad;
 if(!drcScan||!xr::layer_wanted(xr::kGamePad)||!xr::frame_acquire(xr::kGamePad,pad))return;
 const VkExtent2D extent=fit(drcScan,pad);
 ComposeQuad pointer[2];size_t n=0;float u,v;bool pressed;
 if(xr::gamepad_pointer(&u,&v,&pressed)) {
  const float size=std::max(6.0f,extent.height*0.022f),x=u*extent.width,y=v*extent.height;
  ComposeQuad& frame=pointer[n++];frame.solid=true;frame.color[3]=0.85f;frame.box={x-size*0.8f,y-size*0.8f,size*1.6f,size*1.6f};
  ComposeQuad& dot=pointer[n++];dot.solid=true;dot.color[0]=1;dot.color[1]=pressed?0.35f:1;dot.color[2]=pressed?0.05f:1;dot.color[3]=1;
  dot.box={x-size*0.5f,y-size*0.5f,size,size};
 }
 compose_layer(R.drc,pad.image,view_of(pad),extent,pad.format,false,pointer,n);
 xr::frame_picture(xr::kGamePad,extent.width,extent.height);
}
}  // namespace

void headset_started() { xr::set_images_release_hook(drop_views); }

HeadsetFrame headset_begin(Surface* tvScan) {
 screen={};
 if(!xr::active()||!xr::frame_begin())return {};
 if(!xr::frame_acquire(xr::kScreen,screen)) { screen={};xr::frame_end();return {}; }
 screenExtent=fit(tvScan,screen);
 return {true,float(screenExtent.width),float(screenExtent.height)};
}

void headset_draw(Surface* drcScan) {
 if(!screen.image)return;
 try {
  compose_layer(R.tv,screen.image,view_of(screen),screenExtent,screen.format,true);
  xr::frame_picture(xr::kScreen,screenExtent.width,screenExtent.height);
  draw_pad(drcScan);
  flush_async();  // the drawing is queued before the images go back to the headset
 }catch(...) { screen={};xr::frame_end();throw; }  // (hands back what is held)
 screen={};
 xr::frame_end();
}

// World mode: the TV picture of this swap is one eye's view. The left eye begins the headset's frame
// (and waits for it), the right eye, a swap later, ends it; the eye's picture is stretched over the
// eye's whole image, which stands for the field of view it was drawn with.
bool headset_eye(const xrworld::Frame& eye,Surface* tvScan,Surface* drcScan) {
 if(!xr::active()||!tvScan)return false;
 if(eye.index==0) { if(!xr::frame_begin())return false; }
 else if(!xr::frame_open())return false;  // its left eye was not drawn
 const int layer=eye.index?xr::kEyeRight:xr::kEyeLeft;
 try {
  xr::Target target;
  if(xr::frame_acquire(layer,target)) {
   compose_picture(*tvScan,R.tv.srgb.load(),target.image,view_of(target),VkExtent2D{target.width,target.height},target.format);
   xr::frame_picture(layer,target.width,target.height);
   xr::frame_eye(eye.index,eye.view);
  }
  if(eye.index==1)draw_pad(drcScan);
  flush_async();
 }catch(...) { xr::frame_end();throw; }
 xr::frame_release(layer);
 if(eye.index==1)xr::frame_end();
 return true;
}
}  // namespace gfxvk
#else
namespace gfxvk {
HeadsetFrame headset_begin(Surface*) { return {}; }
void headset_draw(Surface*) {}
void headset_started() {}
bool headset_eye(const xrworld::Frame&,Surface*,Surface*) { return false; }
}  // namespace gfxvk
#endif
