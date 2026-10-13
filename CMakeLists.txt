cmake_minimum_required(VERSION 3.20)
project(mecmp LANGUAGES C CXX)

# Mirror's Edge Catalyst is a 64-bit game.
if(NOT CMAKE_SIZEOF_VOID_P EQUAL 8)
  message(FATAL_ERROR "Build for x64 (Visual Studio: choose the x64 configuration).")
endif()

set(CMAKE_CXX_STANDARD 17)
set(CMAKE_CXX_STANDARD_REQUIRED ON)

if(MSVC)
  # Static CRT: no dependency on the VC++ redistributable.
  set(CMAKE_MSVC_RUNTIME_LIBRARY "MultiThreaded$<$<CONFIG:Debug>:Debug>")
  add_compile_options(/W4 /permissive- /utf-8)
  add_compile_definitions(_CRT_SECURE_NO_WARNINGS NOMINMAX)
else()
  add_compile_options(-Wall -Wextra)
  if(WIN32)
    add_link_options(-static -static-libgcc -static-libstdc++)
    add_compile_definitions(NOMINMAX)
  else()
    add_link_options(-static-libgcc -static-libstdc++)
  endif()
endif()

# --- mecmp_server: dedicated server (Windows or Linux, no game needed) ---------------------------
find_package(Threads REQUIRED)
add_executable(mecmp_server tools/server/mecmp_server.cpp src/server/relay.cpp src/net/protocol.cpp src/net/joincode.cpp)
target_include_directories(mecmp_server PRIVATE src)
target_link_libraries(mecmp_server PRIVATE Threads::Threads)
if(WIN32)
  target_link_libraries(mecmp_server PRIVATE ws2_32)
endif()

# Everything else is the Windows mod. On other systems only the server is built.
if(NOT WIN32)
  return()
endif()

# --- mecmp.dll: the mod ------------------------------------------------------------------------
add_library(mecmp SHARED
  src/app/dllmain.cpp
  src/app/mod.cpp
  src/app/diagnostics.cpp
  src/core/config.cpp
  src/core/log.cpp
  src/core/typeinfo.cpp
  src/game/camera.cpp
  src/game/camera_finder.cpp
  src/game/fingerprint.cpp
  src/game/hooks.cpp
  src/game/level.cpp
  src/game/ghosts.cpp
  src/game/models.cpp
  src/game/mumble_link.cpp
  src/game/teleport.cpp
  src/game/local_player.cpp
  src/game/ping_point.cpp
  src/net/net.cpp
  src/net/protocol.cpp
  src/net/upnp.cpp
  src/net/joincode.cpp
  src/ui/overlay.cpp
  src/ui/menu.cpp
  src/ui/gamepad.cpp
  src/ui/nametags.cpp
  src/ui/notices.cpp
  src/ui/chat.cpp
  src/ui/markers.cpp
  src/ui/glyph.cpp
  src/ui/radial.cpp
  third_party/imgui/imgui.cpp
  third_party/imgui/imgui_draw.cpp
  third_party/imgui/imgui_tables.cpp
  third_party/imgui/imgui_widgets.cpp
  third_party/imgui/backends/imgui_impl_dx11.cpp
  third_party/imgui/backends/imgui_impl_win32.cpp
)
target_include_directories(mecmp PRIVATE src third_party/imgui)
# Developer hotkeys (F7 spawn/show ghosts, F8 next model, F9 status, F12 hide). Not configurable and
# not part of release builds: configure release builds with -DMECMP_DEBUG_KEYS=OFF.
option(MECMP_DEBUG_KEYS "Developer hotkeys F7/F8/F9/F12 (OFF for release builds)" ON)
if(MECMP_DEBUG_KEYS)
  target_compile_definitions(mecmp PRIVATE MECMP_DEBUG_KEYS=1)
endif()
target_link_libraries(mecmp PRIVATE ws2_32 d3d11 dxgi d3dcompiler dwmapi)
set_target_properties(mecmp PROPERTIES OUTPUT_NAME "mecmp" PREFIX "")

# --- mecmp_inject.exe: loads mecmp.dll into the running game -------------------------------------
add_executable(mecmp_inject tools/inject/mecmp_inject.cpp)
target_compile_definitions(mecmp_inject PRIVATE UNICODE _UNICODE)
if(MSVC)
  target_link_options(mecmp_inject PRIVATE /ENTRY:wmainCRTStartup)
else()
  target_link_options(mecmp_inject PRIVATE -municode)
endif()

# --- version.dll: optional proxy loader (auto-loads mecmp.dll from the game folder) ---------------
add_library(version_proxy SHARED tools/proxy/version_proxy.cpp tools/proxy/version.def)
set_target_properties(version_proxy PROPERTIES OUTPUT_NAME "version" PREFIX "")
