# VR headsets through OpenXR (runtime/src/xr, docs/vr.md): the Khronos loader and headers. An installed
# package is used where there is one (find_package(OpenXR)); otherwise the pinned source release is
# built with the project's own compiler, like the other dependencies (cmake/WindowsDependencies.cmake).
# Windows links the loader statically: the game imports nothing from an OpenXR runtime and starts the
# same on a computer without one.
#
# On by default on Windows, where the port is tested with a headset (Meta Quest Link); elsewhere
# -DWWHD_OPENXR=ON builds it. It needs the Vulkan renderer with the SDL host.
if(WIN32)
  set(WWHD_OPENXR_DEFAULT ON)
else()
  set(WWHD_OPENXR_DEFAULT OFF)
endif()
option(WWHD_OPENXR "VR headsets through OpenXR (Vulkan renderer)" ${WWHD_OPENXR_DEFAULT})
if(WWHD_OPENXR AND NOT WWHD_SDL_HOST)
  message(STATUS "WWHD_OPENXR needs the Vulkan renderer with the SDL host: off")
  set(WWHD_OPENXR OFF)
endif()
if(NOT WWHD_OPENXR)
  return()
endif()

if(NOT WWHD_BUNDLED_DEPS)
  find_package(OpenXR CONFIG QUIET)
endif()
if(NOT TARGET OpenXR::openxr_loader)
  include(FetchContent)
  if(POLICY CMP0135)
    cmake_policy(SET CMP0135 NEW)
  endif()
  set(BUILD_TESTS OFF CACHE BOOL "" FORCE)
  set(BUILD_API_LAYERS OFF CACHE BOOL "" FORCE)
  set(BUILD_CONFORMANCE_TESTS OFF CACHE BOOL "" FORCE)
  set(BUILD_SDK_TESTS OFF CACHE BOOL "" FORCE)
  FetchContent_Declare(openxr
    URL https://github.com/KhronosGroup/OpenXR-SDK/archive/refs/tags/release-1.1.63.tar.gz
    URL_HASH SHA256=5b8dac0608f49498fe8e95f562fe6bcf0bc781e561f137348a8c251d06a88b6f)
  FetchContent_MakeAvailable(openxr)
endif()
message(STATUS "WWHD OpenXR: on (VR headsets, docs/vr.md)")
