// gpuguard.h — the ONLY interface between game code and the GPU.  [VERSION 2]
//
// *** VERSION 2: the RULE-REWARD ENGINE. ***
// v2 removes v1's raw write channel. There is NO gg_submit(SET/ADD): the host
// cannot name a protected value. It can only submit a TRIGGER (MOVE, SHOOT,
// ATTACK, ...), and the persistent engine decides the resulting state from its
// OWN authoritative prior state plus the rule table in game_rules.cuh.
//
// WHAT CHANGED FROM V1, AND WHY IT MATTERS:
//   v1: gg_submit(slot, SET, 9999) -> engine writes 9999. Injected in-process
//       code (T1) calls the same function and forges any value, no alert. That is
//       the confused-deputy / direct-forge hole.
//   v2: there is no such call. The strongest thing T1 can forge is a RULE-VALID
//       trigger. It still cannot set score=9999; score only rises when the GPU
//       itself adjudicates a kill. See DESIGN_v2.md for the precise invariant and,
//       crucially, for what this does NOT buy you (caller authentication, rate
//       cheats, and -- in GG_MODE_BOUNDS -- max-damage-every-swing).
//
// This header is deliberately free of every CUDA token AND of the Trig/Rules
// structs, so game.cpp/protected.cpp build with a plain g++/clang++ while
// gpuguard.cu is compiled by nvcc. Trigger args are passed as plain ints; the .cu
// packs them into a Trig. The trigger opcodes live in game_rules.cuh (plain C++).

#ifndef GPUGUARD_H
#define GPUGUARD_H

#include <stdint.h>

#ifdef __cplusplus
extern "C" {
#endif

// Export attribute for the one symbol the T1 demo resolves at runtime
// (gg_trigger). It MUST sit on the declaration here so the declaration and the
// definition in gpuguard.cu agree -- MSVC rejects a definition that ADDS
// dllexport the declaration lacked. On non-Windows it expands to nothing.
#ifdef _WIN32
#define GG_API __declspec(dllexport)
#else
#define GG_API
#endif

// Sized to one block. One engine, SLOT_COUNT protected values; see game_rules.cuh.
#define GG_MAX_SLOTS 256

// Verdicts returned by gg_wait(): did the engine APPLY the transition or REJECT it
// (ammo empty, out of bounds, stale CAS, illegal damage, unknown op...)?
enum { GG_PENDING = 0, GG_APPLIED = 1, GG_REJECTED = 2 };

// Bring up CUDA and prepare buffers. Returns 0 on success, non-zero if no usable
// GPU. HARD CONSTRAINT: while the engine is resident, NEVER call
// cudaDeviceSynchronize() -- the engine never finishes, so it blocks forever.
int  gg_init(void);

// Stop the engine and release everything. Safe to call twice.
void gg_shutdown(void);

// End trusted startup and launch the engine. Call ONCE after all protected values
// are allocated (their initials are baked into the GPU at launch) and before the
// game loop. After launch the game drives state ONLY through gg_trigger().
void gg_start(void);

// 1 if the GPU is really guarding, 0 if linked against the CPU stub. Print at
// startup so a stub build (which protects nothing) cannot be mistaken for real.
int  gg_active(void);

// Select the combat verification mode BEFORE gg_start(). 0 = GG_MODE_REDERIVE
// (GPU recomputes damage, true authority), 1 = GG_MODE_BOUNDS (GPU trusts a
// bounded CPU claim). Also settable via env var GG_COMBAT_MODE. This is the core
// experiment lever. No effect after launch (the rule table is then device-resident).
void gg_set_combat_mode(int mode);
int  gg_combat_mode(void);

// Reserve a slot and seed it, during trusted startup only. Returns -1 when slots
// are exhausted or called after gg_start(). The game allocates exactly SLOT_COUNT
// slots, in the SLOT_* order from game_rules.cuh.
int  gg_alloc_slot(uint32_t initial);

// The address the game READS a protected value through -- a mirror page the engine
// heals every iteration. In v2 the host never writes it; an external editor (T0)
// that pokes it is detected (divergence from the shadow) and healed. This is the
// address a memory editor would target.
volatile uint32_t *gg_slot_ptr(int slot);

// *** THE ONLY WRITE-SIDE ENTRY POINT. *** Queue a trigger and return its sequence
// number. Does NOT wait. The engine validates it against its authoritative state
// and the rule table, then applies or rejects. Injected code (T1) can call this
// too -- but, unlike v1's gg_submit, the worst it achieves is a RULE-VALID
// transition, never an arbitrary value. (On Windows this is exported for the T1
// demo, exactly as v1 exported gg_submit; see gpuguard.cu.)
GG_API uint64_t gg_trigger(int op, int32_t arg0, int32_t arg1, int32_t arg2, int32_t arg3);

// Block until the engine has consumed `seq`, then return its verdict
// (GG_APPLIED / GG_REJECTED). GG_PENDING only if seq is invalid.
int  gg_wait(uint64_t seq);

// Poll once per frame. Catches EXTERNAL edits of the mirror page (T0). It does NOT
// and cannot flag a rule-valid trigger from injected code -- that is not tamper,
// it is a legal transition, which is the whole point of the v2 threat model.
int  gg_tampered(void);
int  gg_tamper_slot(void);

// Engine counters for the demo's `status` command. gg_mismatch() is re-derive-mode
// telemetry: how often the CPU's claimed damage disagreed with the GPU's.
uint32_t gg_applied(void);
uint32_t gg_rejected(void);
uint32_t gg_mismatch(void);

// Addresses of the guard's own state, for the `addrs` demo command -- what a
// memory editor can and cannot reach. Return NULL before init.
const volatile void *gg_bell_addr(void);   // doorbell (tail), DEVICE memory
const void          *gg_ctl_addr(void);    // control block (GPU-write/host-read)
const void          *gg_ring_devptr(void); // request ring, DEVICE memory

#ifdef __cplusplus
}
#endif
#endif
