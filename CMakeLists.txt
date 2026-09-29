# Cross-platform build for the v1 GPU anti-cheat (Windows MSVC+CUDA, and Linux).
#
#   cmake -B build
#   cmake --build build --config Release
#
# Produces:  build/Release/game.exe   (Windows)   or   build/game   (Linux)
#
# On Windows, run this from a normal terminal after installing Visual Studio 2022
# (Desktop development with C++) and the CUDA Toolkit; CMake finds cl.exe and nvcc.
# If CMake is older than 3.24 and rejects "native", pass your GPU's arch instead:
#   cmake -B build -DCMAKE_CUDA_ARCHITECTURES=86      (86 = RTX 30xx Ampere)

cmake_minimum_required(VERSION 3.20)
project(gpu_anticheat_v1 LANGUAGES CXX CUDA)

set(CMAKE_CXX_STANDARD 17)
set(CMAKE_CXX_STANDARD_REQUIRED ON)
set(CMAKE_CUDA_STANDARD 17)

if(NOT DEFINED CMAKE_CUDA_ARCHITECTURES)
  set(CMAKE_CUDA_ARCHITECTURES native)   # auto-detect the installed GPU (CMake >= 3.24)
endif()

# The real GPU build: nvcc compiles the .cu, MSVC/gcc the .cpp, nvcc links.
add_executable(game game.cpp protected.cpp gpuguard.cu)

find_package(Threads REQUIRED)
target_link_libraries(game PRIVATE Threads::Threads)
