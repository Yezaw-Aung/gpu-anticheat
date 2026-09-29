# code/game-v1 — build  (VERSION 1: the write channel)
#
#   make          GPU build   (needs nvcc; this is the real thing)
#   make cpu      CPU stub    (no CUDA; logic only, guards nothing)
#   make clean
#
# If nvcc is older than 11.5, -arch=native does not exist. Override it:
#   make ARCH=sm_86        (RTX 30xx)   sm_75 (Turing)   sm_61 (Pascal)

CUDA_HOME ?= /usr/local/cuda
NVCC      ?= $(CUDA_HOME)/bin/nvcc
CXX       ?= g++
ARCH      ?= native

CXXFLAGS  := -O2 -std=c++17 -Wall -Wextra -pthread
NVCCFLAGS := -O2 -std=c++17 -arch=$(ARCH) -Xcompiler -Wall

# Game code is compiled by the ordinary C++ compiler; only gpuguard.cu sees nvcc.
# nvcc performs the final link so we do not have to locate libcudart by hand.

all: game

game: game.o protected.o gpuguard.o
	$(NVCC) $(NVCCFLAGS) $^ -o $@ -Xcompiler -pthread

gpuguard.o: gpuguard.cu gpuguard.h
	$(NVCC) $(NVCCFLAGS) -c $< -o $@

game.o: game.cpp protected.h gpuguard.h
	$(CXX) $(CXXFLAGS) -c $< -o $@

protected.o: protected.cpp protected.h gpuguard.h
	$(CXX) $(CXXFLAGS) -c $< -o $@

# ---- CPU stub: builds anywhere, protects nothing --------------------------
cpu: game_cpu

game_cpu: game.cpp protected.cpp gpuguard_stub.cpp protected.h gpuguard.h
	$(CXX) $(CXXFLAGS) game.cpp protected.cpp gpuguard_stub.cpp -o $@ -pthread

clean:
	rm -f game game_cpu *.o
	rm -rf build

.PHONY: all cpu clean
