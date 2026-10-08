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
  add_link_options(-static -static-libgcc -static-libstdc++)
  add_compile_definitions(NOMINMAX)
endif()

# --- mecmp.dll: the mod ------------------------------------------------------------------------
add_library(mecmp SHARED
  src/app/dllmain.cpp
  src/app/mod.cpp
  src/app/diagnostics.cpp
  src/core/config.cpp
  src/core/log.cpp
  src/core/typeinfo.cpp
  src/game/hooks.cpp
  src/game/level.cpp
  src/game/ghosts.cpp
  src/game/models.cpp
  src/game/local_player.cpp
  src/net/net.cpp
  src/net/protocol.cpp
  src/net/upnp.cpp
  src/net/joincode.cpp
)
target_include_directories(mecmp PRIVATE src)
target_link_libraries(mecmp PRIVATE ws2_32)
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
