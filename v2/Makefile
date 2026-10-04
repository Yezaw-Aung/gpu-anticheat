# v2 — build  (VERSION 2: the rule-reward engine)
#
#   make          GPU build   (needs nvcc; this is the real thing)
#   make cpu      CPU stub    (no CUDA; logic only, guards nothing)
#   make clean
#
# Override the arch if -arch=native is unavailable (nvcc < 11.5):
#   make ARCH=sm_86        (RTX 30xx)   sm_75 (Turing)   sm_61 (Pascal)

CUDA_HOME ?= /usr/local/cuda
NVCC      ?= $(CUDA_HOME)/bin/nvcc
CXX       ?= g++
ARCH      ?= native

CXXFLAGS  := -O2 -std=c++17 -Wall -Wextra -pthread
NVCCFLAGS := -O2 -std=c++17 -arch=$(ARCH) -Xcompiler -Wall

# Only gpuguard.cu sees nvcc; game.cpp is plain C++. nvcc does the final link.
all: game

game: game.o gpuguard.o
	$(NVCC) $(NVCCFLAGS) $^ -o $@ -Xcompiler -pthread

gpuguard.o: gpuguard.cu gpuguard.h game_rules.cuh
	$(NVCC) $(NVCCFLAGS) -c $< -o $@

game.o: game.cpp gpuguard.h game_rules.cuh protected.h
	$(CXX) $(CXXFLAGS) -c $< -o $@

# ---- CPU stub: builds anywhere, protects nothing --------------------------
cpu: game_cpu

game_cpu: game.cpp gpuguard_stub.cpp gpuguard.h game_rules.cuh protected.h
	$(CXX) $(CXXFLAGS) game.cpp gpuguard_stub.cpp -o $@ -pthread

clean:
	rm -f game game_cpu *.o
	rm -rf build

.PHONY: all cpu clean
