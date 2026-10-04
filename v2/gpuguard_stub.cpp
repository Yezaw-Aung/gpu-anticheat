// gpuguard_stub.cpp — a no-GPU implementation of the v2 C surface.  [VERSION 2]
//
// PROVIDES NO PROTECTION WHATSOEVER. It exists so the game builds and its logic
// runs on a machine without CUDA (e.g. a laptop) before being taken to the GPU
// box. gg_active() returns 0 and the demo prints an UNPROTECTED banner.
//
// Semantics are IDENTICAL to the engine because the stub calls the SAME
// gg_apply_rule() from game_rules.cuh -- the whole point of keeping the rule core
// in a shared, toolchain-neutral header. The difference is only WHERE the shadow
// lives: here it is a plain host array any code (including a "cheat") can edit, so
// nothing is actually enforced. The rules still run, so the economy behaves the
// same; they just aren't defended.

#include <cstdio>
#include <cstdlib>
#include "gpuguard.h"
#include "game_rules.cuh"

namespace {
uint32_t g_shadow[GG_MAX_SLOTS];   // the "authoritative" state -- but in plain host
                                   // memory, so this build protects nothing.
int      g_next = 0;
uint64_t g_seq  = 0;
Rules    g_rules;
bool     g_inited = false;
uint32_t g_applied = 0, g_rejected = 0, g_mismatch = 0;
uint8_t  g_verdict[256] = {0};   // per-seq verdict ring, so gg_wait is honest

void ensure_rules() {
    if (g_inited) return;
    gg_default_rules(&g_rules);
    if (const char *m = getenv("GG_COMBAT_MODE"))
        g_rules.combat_mode = (atoi(m) != 0) ? GG_MODE_BOUNDS : GG_MODE_REDERIVE;
    g_inited = true;
}
}

extern "C" int  gg_init(void)     { ensure_rules(); return 0; }
extern "C" void gg_start(void)    { ensure_rules(); }
extern "C" void gg_shutdown(void) { fflush(stdout); }
extern "C" int  gg_active(void)   { return 0; }

extern "C" void gg_set_combat_mode(int mode) { ensure_rules(); g_rules.combat_mode = mode ? GG_MODE_BOUNDS : GG_MODE_REDERIVE; }
extern "C" int  gg_combat_mode(void)         { ensure_rules(); return (int)g_rules.combat_mode; }

extern "C" int gg_alloc_slot(uint32_t initial)
{
    if (g_next >= GG_MAX_SLOTS) return -1;
    int s = g_next++;
    g_shadow[s] = initial;
    return s;
}

extern "C" volatile uint32_t *gg_slot_ptr(int slot)
{
    if (slot < 0 || slot >= GG_MAX_SLOTS) return nullptr;
    return (volatile uint32_t *)&g_shadow[slot];
}

// The trigger channel, run straight on the host "shadow". Same rule as the engine,
// no protection (this is the stub).
extern "C" uint64_t gg_trigger(int op, int32_t a0, int32_t a1, int32_t a2, int32_t a3)
{
    ensure_rules();
    Trig t; t.seq = (uint32_t)g_seq; t.op = (uint32_t)op;
    t.arg0 = a0; t.arg1 = a1; t.arg2 = a2; t.arg3 = a3;
    uint32_t mism = 0;
    int ok = gg_apply_rule(g_shadow, &g_rules, &t, &mism);
    if (ok) g_applied++; else g_rejected++;
    g_mismatch += mism;
    g_verdict[g_seq % 256] = ok ? GG_APPLIED : GG_REJECTED;
    return g_seq++;
}

extern "C" int gg_wait(uint64_t seq)
{
    if (seq == UINT64_MAX) return GG_PENDING;
    return (int)g_verdict[seq % 256];
}

extern "C" int      gg_tampered(void)    { return 0; }   // cannot detect anything
extern "C" int      gg_tamper_slot(void) { return -1; }
extern "C" uint32_t gg_applied(void)     { return g_applied; }
extern "C" uint32_t gg_rejected(void)    { return g_rejected; }
extern "C" uint32_t gg_mismatch(void)    { return g_mismatch; }

extern "C" const volatile void *gg_bell_addr(void)  { return nullptr; }
extern "C" const void          *gg_ctl_addr(void)    { return nullptr; }
extern "C" const void          *gg_ring_devptr(void) { return nullptr; }
