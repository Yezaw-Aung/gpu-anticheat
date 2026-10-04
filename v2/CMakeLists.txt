# Cross-platform build for the v2 GPU rule-reward anti-cheat (Windows MSVC+CUDA, Linux).
#
#   cmake -B build
#   cmake --build build --config Release
#
# Produces:  build/Release/game.exe (Windows)  or  build/game (Linux)
#
# On Windows install Visual Studio 2022 (Desktop C++) + CUDA Toolkit; CMake finds
# cl.exe and nvcc. If CMake < 3.24 rejects "native", pass the arch:
#   cmake -B build -DCMAKE_CUDA_ARCHITECTURES=86      (86 = RTX 30xx Ampere)

cmake_minimum_required(VERSION 3.20)
project(gpu_anticheat_v2 LANGUAGES CXX CUDA)

set(CMAKE_CXX_STANDARD 17)
set(CMAKE_CXX_STANDARD_REQUIRED ON)
set(CMAKE_CUDA_STANDARD 17)

if(NOT DEFINED CMAKE_CUDA_ARCHITECTURES)
  set(CMAKE_CUDA_ARCHITECTURES native)   # auto-detect (CMake >= 3.24)
endif()

# nvcc compiles the .cu, the host compiler the .cpp, nvcc links. protected.h is
# header-only and game_rules.cuh is shared by both toolchains.
add_executable(game game.cpp gpuguard.cu)

find_package(Threads REQUIRED)
target_link_libraries(game PRIVATE Threads::Threads)
