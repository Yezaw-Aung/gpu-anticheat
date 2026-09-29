// gpuguard.h — the ONLY interface between game code and the GPU.  [VERSION 1]
//
// *** VERSION 1: the raw WRITE CHANNEL. ***
// This is the original design. The game changes a protected value by submitting a
// value operation on its slot -- gg_submit(slot, GG_OP_SET/GG_OP_ADD, arg) -- and
// the persistent engine applies it to that slot's register shadow. It is simple
// and scales trivially to any value (a Protected just wraps a slot and forwards
// its +=/-=/= to gg_submit). See code/game/ for VERSION 2, the rule-reward engine
// that removes this file's central weakness.
//
// WHAT V1 DEFENDS, AND WHAT IT DOES NOT.
//   * Defends (T0, external editor): an edit of the plaintext page diverges from
//     the register shadow and is caught by the tamper poll. Same as v2.
//   * Does NOT defend (T1, in-process code): the engine cannot tell a legitimate
//     caller from injected code issuing the SAME gg_submit. Injected code can call
//     gg_submit(slot, GG_OP_SET, 9999) and the engine applies it -- no divergence,
//     no alert. This is the direct-forge / confused-deputy hole. v2 closes it by
//     removing the raw write path entirely (trigger-only, GPU-owned rewards).
//
// This header is deliberately free of every CUDA token, so game.cpp/protected.cpp
// build with a plain g++/clang++ while gpuguard.cu is compiled by nvcc.

#ifndef GPUGUARD_H
#define GPUGUARD_H

#include <stdint.h>

#ifdef __cplusplus
extern "C" {
#endif

// The value operations the write channel understands. (No TRIGGER in v1 -- there
// are no rules; the host computes amounts and submits them.)
enum { GG_OP_ADD = 0, GG_OP_SET = 1 };

// Sized to one 256-thread block: one thread per slot, so every shadow stays
// in a register. Raising this past 1024 needs a different kernel shape.
#define GG_MAX_SLOTS 256

// Brings up the CUDA context and launches the persistent engine immediately.
// Returns 0 on success, non-zero if no usable GPU was found.
//
// *** HARD CONSTRAINT ON THE HOST PROGRAM ***
// While the engine is resident, the process must NEVER call
// cudaDeviceSynchronize() -- it waits for all device work, and the engine never
// finishes, so the call blocks forever. Synchronise per stream instead.
int  gg_init(void);

// Stops the engine and releases everything. Safe to call twice.
void gg_shutdown(void);

// Ends trusted startup and launches the engine. Call ONCE after all protected
// values are allocated (their initial values are baked into the GPU at launch)
// and before the game loop. After launch, the game changes a protected value by
// submitting a SET/ADD on its slot (the write channel).
void gg_start(void);

// 1 if the GPU is really guarding, 0 if this binary was linked against the CPU
// stub. Print it at startup so a stub build cannot be mistaken for a real one.
int  gg_active(void);

// Reserves a slot and seeds it. Returns -1 when slots are exhausted. Must be
// called during trusted startup, before gg_start().
int  gg_alloc_slot(uint32_t initial);

// The address the game reads through — and the address a memory editor would
// target. Lives in mapped pinned host memory, so the read is a plain load.
volatile uint32_t *gg_slot_ptr(int slot);

// THE WRITE CHANNEL. Queues a value mutation on a slot and returns its sequence
// number. Does NOT wait -- pair with gg_wait() when you need the result before
// reading. This is how the game (and, in v1, an in-process attacker) changes a
// protected value.
uint64_t gg_submit(int slot, int op, int32_t arg);

// Blocks until the engine has consumed the request with that sequence number.
void gg_wait(uint64_t seq);

// Poll once per frame. gg_tamper_slot() is meaningful only when this is 1. Note
// this catches external EDITS, not forged submits (a submit updates the shadow
// too, so it leaves no divergence -- the v1 hole).
int  gg_tampered(void);
int  gg_tamper_slot(void);

// Engine counters, for the demo's `status` command.
uint32_t gg_applied(void);
uint32_t gg_rejected(void);

// Addresses of the guard's own state, for the `addrs` demo command. They show
// what an external memory editor can and cannot reach. Return NULL before init.
const volatile void *gg_bell_addr(void);   // the doorbell (tail), DEVICE memory
const void          *gg_ctl_addr(void);    // the control block (GPU-write/host-read)
const void          *gg_stage_addr(void);  // staging ring (recent requests), host
const void          *gg_ring_devptr(void); // the request ring, DEVICE memory

#ifdef __cplusplus
}
#endif
#endif
