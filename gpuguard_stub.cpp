// gpuguard_stub.cpp — a no-GPU implementation of the same C surface.  [VERSION 1]
//
// PROVIDES NO PROTECTION WHATSOEVER. It exists so the game can be built and its
// logic exercised on a machine without CUDA (e.g. a Mac laptop) before being taken
// to the GPU box. gg_active() returns 0 and the demo prints an UNPROTECTED banner,
// so a stub build cannot be mistaken for a real one.
//
// Unlike the v2 stub, gg_submit here ACTUALLY APPLIES the SET/ADD -- because in v1
// the write channel is the real update path, so the economy must work in the stub.

#include <cstdio>
#include "gpuguard.h"

namespace {
uint32_t g_val[GG_MAX_SLOTS];
int      g_next = 0;
uint64_t g_seq  = 0;
}

extern "C" int  gg_init(void)     { return 0; }
extern "C" void gg_start(void)    { }
extern "C" void gg_shutdown(void) { fflush(stdout); }
extern "C" int  gg_active(void)   { return 0; }

extern "C" int gg_alloc_slot(uint32_t initial)
{
    if (g_next >= GG_MAX_SLOTS) return -1;
    int s = g_next++;
    g_val[s] = initial;
    return s;
}

extern "C" volatile uint32_t *gg_slot_ptr(int slot)
{
    if (slot < 0 || slot >= GG_MAX_SLOTS) return nullptr;
    return (volatile uint32_t *)&g_val[slot];
}

// The write channel, run straight on the plaintext. No protection (this is the
// stub), but the SET/ADD semantics match the engine so the economy is identical.
extern "C" uint64_t gg_submit(int slot, int op, int32_t arg)
{
    if (slot < 0 || slot >= GG_MAX_SLOTS) return UINT64_MAX;
    if      (op == GG_OP_SET) g_val[slot]  = (uint32_t)arg;
    else if (op == GG_OP_ADD) g_val[slot] += (uint32_t)arg;
    return g_seq++;
}

extern "C" void     gg_wait(uint64_t)    { }
extern "C" int      gg_tampered(void)    { return 0; }   // cannot detect anything
extern "C" int      gg_tamper_slot(void) { return -1; }
extern "C" uint32_t gg_applied(void)     { return (uint32_t)g_seq; }
extern "C" uint32_t gg_rejected(void)    { return 0; }

extern "C" const volatile void *gg_bell_addr(void)  { return nullptr; }
extern "C" const void          *gg_ctl_addr(void)    { return nullptr; }
extern "C" const void          *gg_stage_addr(void)  { return nullptr; }
extern "C" const void          *gg_ring_devptr(void) { return nullptr; }
