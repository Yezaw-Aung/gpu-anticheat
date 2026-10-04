# Cross-platform CMake build for the v2 GPU rule-reward anti-cheat.
# Primary build path on Windows (MSVC + CUDA Toolkit) and on Linux.
#
# ─────────────────────────────────────────────────────────────────────────────
# WINDOWS QUICK START  (normal PowerShell or cmd; CUDA Toolkit + VS 2022 installed)
#
#   cmake -B build                            :: configure (auto-detects your GPU)
#   cmake --build build --config Release      :: build everything
#
#   Run:  .\build\Release\game.exe
#   T1 demo (separate terminal):
#         .\build\Release\injector.exe game.exe .\build\Release\cheat.dll
#
#   If CMake < 3.24 rejects "native", pass your GPU arch (86 = RTX 30xx Ampere):
#         cmake -B build -DCMAKE_CUDA_ARCHITECTURES=86
#
#   *** The persistent engine needs the GPU watchdog (TDR) disabled, or Windows
#       kills the long-running kernel after ~2 s. See v2/README.md. ***
#
# ─────────────────────────────────────────────────────────────────────────────
# TARGETS
#   game        the real GPU build (game.cpp + gpuguard.cu)      [needs CUDA]
#   game_cpu    CPU stub, no CUDA, guards nothing (dev/laptop)   [always]
#   cheat       attack/cheat.cpp -> cheat.dll (T1 demo)          [Windows only]
#   injector    attack/injector.cpp -> injector.exe             [Windows only]
#
# Toggle the attack demo with -DBUILD_ATTACK_DEMO=OFF.
# ─────────────────────────────────────────────────────────────────────────────

cmake_minimum_required(VERSION 3.20)

# CXX is always required; CUDA is enabled only if a compiler is found, so this
# project still configures (stub + attack demo) on a machine without the toolkit.
project(gpu_anticheat_v2 LANGUAGES CXX)

set(CMAKE_CXX_STANDARD 17)
set(CMAKE_CXX_STANDARD_REQUIRED ON)

option(BUILD_ATTACK_DEMO "Build the T1 cheat.dll + injector.exe demo (Windows)" ON)
option(BUILD_GRAPHICS    "Build the raylib graphical frontend (fetched by CMake)" ON)

# Put every binary in one place so the injector and cheat.dll sit next to game.exe.
set(CMAKE_RUNTIME_OUTPUT_DIRECTORY ${CMAKE_BINARY_DIR})
set(CMAKE_LIBRARY_OUTPUT_DIRECTORY ${CMAKE_BINARY_DIR})

# ── detect CUDA ──────────────────────────────────────────────────────────────
include(CheckLanguage)
check_language(CUDA)
if(CMAKE_CUDA_COMPILER)
  enable_language(CUDA)
  set(CMAKE_CUDA_STANDARD 17)
  if(NOT DEFINED CMAKE_CUDA_ARCHITECTURES)
    set(CMAKE_CUDA_ARCHITECTURES native)   # auto-detect the installed GPU (CMake >= 3.24)
  endif()
  set(GG_HAVE_CUDA TRUE)
else()
  message(WARNING
    "No CUDA compiler found: building the CPU STUB only (game_cpu), which "
    "PROTECTS NOTHING. Install the CUDA Toolkit to build the real 'game' target.")
  set(GG_HAVE_CUDA FALSE)
endif()

find_package(Threads REQUIRED)

# ── raylib (graphical frontend) ──────────────────────────────────────────────
# Fetched and built from source by CMake, so there is nothing to install. On
# Windows it bundles everything it needs; on Linux it wants the usual X11/GL dev
# packages. Turn it off with -DBUILD_GRAPHICS=OFF if you only want the terminal
# and stub builds.
if(BUILD_GRAPHICS)
  include(FetchContent)
  set(FETCHCONTENT_QUIET OFF)
  set(BUILD_EXAMPLES OFF CACHE BOOL "" FORCE)
  set(BUILD_GAMES    OFF CACHE BOOL "" FORCE)
  FetchContent_Declare(
    raylib
    GIT_REPOSITORY https://github.com/raysan5/raylib.git
    GIT_TAG        5.5
    GIT_SHALLOW    TRUE
  )
  FetchContent_MakeAvailable(raylib)
endif()

# ── the real GPU build ───────────────────────────────────────────────────────
# nvcc compiles gpuguard.cu, the host compiler compiles game.cpp, nvcc links.
# game_rules.cuh is shared by both; protected.h is header-only.
if(GG_HAVE_CUDA)
  add_executable(game game.cpp gpuguard.cu)
  target_link_libraries(game PRIVATE Threads::Threads)
  # On Windows the engine exports gg_trigger (via __declspec(dllexport) in the .cu)
  # so the injected cheat.dll can resolve it. Also export it on the whole exe so a
  # GetProcAddress(GetModuleHandle(NULL), "gg_trigger") succeeds for the demo.
  if(WIN32)
    set_target_properties(game PROPERTIES ENABLE_EXPORTS ON)
  endif()
endif()

# ── CPU stub (builds anywhere; guards nothing) ───────────────────────────────
add_executable(game_cpu game.cpp gpuguard_stub.cpp)
target_link_libraries(game_cpu PRIVATE Threads::Threads)

# ── graphical frontend (raylib) ──────────────────────────────────────────────
# game_gfx     : graphical + real GPU engine   [needs CUDA + graphics]
# game_gfx_cpu : graphical + CPU stub           [needs graphics]
if(BUILD_GRAPHICS)
  if(GG_HAVE_CUDA)
    add_executable(game_gfx game_gfx.cpp gpuguard.cu)
    target_link_libraries(game_gfx PRIVATE raylib Threads::Threads)
    if(WIN32)
      set_target_properties(game_gfx PROPERTIES ENABLE_EXPORTS ON)  # export gg_trigger for the T1 demo
    endif()
  endif()

  add_executable(game_gfx_cpu game_gfx.cpp gpuguard_stub.cpp)
  target_link_libraries(game_gfx_cpu PRIVATE raylib Threads::Threads)
endif()

# ── T1 attack demo (Windows only) ────────────────────────────────────────────
if(BUILD_ATTACK_DEMO AND WIN32)
  add_library(cheat SHARED attack/cheat.cpp)
  target_link_libraries(cheat PRIVATE user32)
  set_target_properties(cheat PROPERTIES PREFIX "" OUTPUT_NAME "cheat")  # -> cheat.dll

  add_executable(injector attack/injector.cpp)
endif()

# ── MSVC niceties ────────────────────────────────────────────────────────────
if(MSVC)
  add_compile_options(/EHsc)
  # The persistent-kernel busy-waits in gg_wait; no special flags needed, but keep
  # warnings sane across the mixed MSVC/nvcc build.
  if(GG_HAVE_CUDA)
    target_compile_options(game PRIVATE $<$<COMPILE_LANGUAGE:CXX>:/W3>)
  endif()
endif()

message(STATUS "gpu-anticheat v2: CUDA=${GG_HAVE_CUDA}  attack-demo=${BUILD_ATTACK_DEMO}  platform=${CMAKE_SYSTEM_NAME}")
