// gpuguard.cu — the persistent rule-reward engine and its C shim.  [VERSION 2]
//
//   The only file nvcc has to see.
//
// *** VERSION 2: the RULE-REWARD ENGINE. ***
// The transport is identical to v1 and is the part that was already sound:
//   1. Shadows are ON-CHIP (shared memory), seeded from the pre-launch mirror
//      page. Not in host memory, not in device GLOBAL memory an attacker could
//      map -- no off-GPU address at all. (v1 used registers; v2 uses shared memory
//      so a single rule can read/write several slots. Same reachability: on-chip,
//      off-limits to T0 and to T1's host-side code.)
//   2. The request ring CONTENTS are in cudaMalloc'd device memory (EFAULT from
//      another process): a request cannot be forged or edited from the host.
//   3. The DOORBELL (tail) is ALSO device memory; the host advances it only via a
//      cudaMemcpyAsync on the SAME stream as the ring-entry copy, so the bump
//      cannot overtake the entry it announces. A register head + per-request seq
//      are the net behind it.
//
// WHAT IS NEW IN V2 is only the engine's inner action: instead of applying a raw
// value op (v1's hole), it evaluates gg_apply_rule() from game_rules.cuh against
// the authoritative shadow. The rule table lives in device memory, seeded once at
// launch, so the host cannot edit the rules after startup.
//
// The rules are evaluated by THREAD 0 alone (requests are strictly ordered, and a
// rule may span several slots), while ALL threads share the tamper poll. See
// DESIGN_v2.md for why single-threaded rule evaluation is the right shape here.

#include <cuda_runtime.h>
#include <cstdio>
#include <cstdlib>
#include <cstring>
#include <mutex>
#include "gpuguard.h"
#include "game_rules.cuh"

#define CK(c) do { cudaError_t e_ = (c); if (e_ != cudaSuccess) {                \
    fprintf(stderr, "[gpuguard] CUDA %s:%d %s\n", __FILE__, __LINE__,            \
            cudaGetErrorString(e_)); fflush(stderr); return -1; } } while (0)

#define CK_V(c) do { cudaError_t e_ = (c); if (e_ != cudaSuccess) {              \
    fprintf(stderr, "[gpuguard] CUDA %s:%d %s\n", __FILE__, __LINE__,            \
            cudaGetErrorString(e_)); fflush(stderr); } } while (0)

#define RING_N   256
#define GG_SLACK 8      // cap in-flight at RING_N - GG_SLACK so the host can't lap

// DEMO ONLY: on Windows, export gg_trigger from game.exe so the injected cheat DLL
// can resolve it with GetProcAddress -- exactly as v1 exported gg_submit. The
// security point is identical: once code runs in the process it can drive the
// game's own trigger channel. The difference v2 demonstrates is that doing so
// yields only RULE-VALID transitions, not arbitrary values.
#ifdef _WIN32
#define GG_EXPORT extern "C" __declspec(dllexport)
#else
#define GG_EXPORT extern "C"
#endif

// GPU-WRITE, HOST-READ only. Every engine INPUT (ring, doorbell, stop flag, rule
// table) is in device memory the host cannot reach; the only host-visible state is
// these OUTPUTS. An attacker can corrupt an output to mislead the host's own
// reporting, but never to change an engine decision. `verdict[]` is the per-request
// result channel for gg_wait().
struct Ctl {
    uint32_t head, applied, rejected, mismatch;   uint8_t p0[112];
    uint32_t tamper, tamper_slot;                 uint8_t p1[120];
    uint8_t  verdict[RING_N];                     uint8_t p2[256 - (RING_N % 256)];
};

// ---------------------------------------------------------------- the engine
__global__ void engine(const Trig *ring, const uint32_t *d_tail,
                       const uint32_t *d_stop, uint32_t *val,
                       const Rules *d_rules, volatile Ctl *c)
{
    const int s = threadIdx.x;

    // The authoritative shadow: on-chip shared memory, seeded from the pre-launch
    // mirror page (which gg_alloc_slot filled with each slot's initial). A private
    // copy of the rule table is cached in shared memory too, so the hot path never
    // re-reads device global.
    __shared__ uint32_t shadow[SLOT_COUNT];
    __shared__ Rules    R;
    __shared__ uint32_t s_tail, s_stop;

    if (s < SLOT_COUNT) shadow[s] = val[s];
    if (s == 0) R = *d_rules;
    __syncthreads();

    uint32_t head = 0;   // thread 0's register copy of the consumed count

    for (;;) {
        if (s == 0) {
            s_tail = *(const volatile uint32_t *)d_tail;   // device doorbell
            s_stop = *(const volatile uint32_t *)d_stop;   // device stop flag
        }
        __syncthreads();
        if (s_stop) break;                        // uniform across the block
        const uint32_t tail = s_tail;

        // THE RULE LOOP. Thread 0 only: requests are ordered and a rule may touch
        // several slots, so serial evaluation is correct and simplest.
        if (s == 0) {
            // Snapshot the authoritative state BEFORE draining, so we can heal the
            // mirror for exactly the slots a RULE changed this iteration -- and
            // leave every other slot untouched, so an external edit to a quiet slot
            // still diverges and is caught by the poll below. This is v1's "heal
            // only what you applied" ordering, generalised to multi-slot rules; it
            // is what lets the poll tell a legitimate change from a tamper.
            uint32_t pre[SLOT_COUNT];
            for (int j = 0; j < SLOT_COUNT; j++) pre[j] = shadow[j];

            for (int i = 0; i < RING_N && (int32_t)(tail - head) > 0; i++) {
                const Trig t = ring[head % RING_N];

                // head is a register the attacker cannot reach: the net if a
                // doorbell bump is ever seen before its entry.
                uint8_t verdict;
                if (t.seq != head) {
                    c->rejected++;
                    verdict = GG_REJECTED;
                } else {
                    uint32_t mism = 0;
                    int ok = gg_apply_rule(shadow, &R, &t, &mism);
                    if (ok) { c->applied++;  verdict = GG_APPLIED; }
                    else    { c->rejected++; verdict = GG_REJECTED; }
                    if (mism) c->mismatch += mism;
                }
                c->verdict[head % RING_N] = verdict;   // result channel for gg_wait
                head++;
            }

            // Heal ONLY the slots a rule actually changed. A slot still divergent
            // after this was changed by someone other than the rules -> the poll
            // below flags it as tamper.
            for (int j = 0; j < SLOT_COUNT; j++)
                if (shadow[j] != pre[j]) val[j] = shadow[j];

            c->head = head;
        }
        __syncthreads();
        __threadfence_system();

        // Tamper poll: mirror page vs. shadow. Catches an EXTERNAL edit (T0) -- it
        // diverges from the shadow. It does NOT catch a rule-valid trigger (that
        // changed the shadow too, legitimately). Each thread polls one slot.
        if (s < SLOT_COUNT && val[s] != shadow[s]) {
            c->tamper = 1;
            c->tamper_slot = (uint32_t)s;
            val[s] = shadow[s];                   // self-heal back to authoritative
        }
    }
}

// ------------------------------------------------------------------ the shim
namespace {

bool          g_up      = false;
bool          g_alloc   = false;
bool          g_failed  = false;
int           g_next    = 0;
uint32_t     *g_val     = nullptr;   // mapped pinned: the mirror page (read side)
Trig         *g_ring    = nullptr;   // device only: request payloads
Trig         *g_stage   = nullptr;   // pinned staging ring (source for the copy)
uint32_t     *g_stail   = nullptr;   // pinned staging for doorbell bumps
uint32_t     *g_tail_dev= nullptr;   // device only: the doorbell (one-way)
uint32_t     *g_stop_dev= nullptr;   // device only: the stop flag
Rules        *g_rules_dev = nullptr; // device only: the rule table
Ctl          *g_ctl     = nullptr;   // mapped pinned control block (GPU-write/host-read)
Rules         g_rules;               // host copy, editable ONLY before launch
uint32_t      g_tail    = 0;         // host producer counter (NOT the GPU doorbell)
cudaStream_t  g_copy    = nullptr;
cudaStream_t  g_kernel  = nullptr;
std::mutex    g_lock;

volatile Ctl *ctl() { return (volatile Ctl *)g_ctl; }

} // namespace

static int alloc_buffers(void)
{
    if (g_alloc)  return 0;
    if (g_failed) return -1;

    int ndev = 0;
    if (cudaGetDeviceCount(&ndev) != cudaSuccess || ndev == 0) {
        fprintf(stderr, "[gpuguard] no CUDA device\n"); fflush(stderr);
        g_failed = true; return -1;
    }
    CK(cudaSetDeviceFlags(cudaDeviceMapHost));

    CK(cudaHostAlloc(&g_val, GG_MAX_SLOTS * sizeof(uint32_t), cudaHostAllocMapped));
    memset(g_val, 0, GG_MAX_SLOTS * sizeof(uint32_t));

    CK(cudaHostAlloc(&g_ctl, sizeof(Ctl), cudaHostAllocMapped));
    memset(g_ctl, 0, sizeof(Ctl));

    CK(cudaHostAlloc(&g_stage, RING_N * sizeof(Trig), cudaHostAllocDefault));
    memset(g_stage, 0, RING_N * sizeof(Trig));
    CK(cudaHostAlloc(&g_stail, RING_N * sizeof(uint32_t), cudaHostAllocDefault));
    memset(g_stail, 0, RING_N * sizeof(uint32_t));

    CK(cudaMalloc(&g_ring, RING_N * sizeof(Trig)));
    CK(cudaMemset(g_ring, 0, RING_N * sizeof(Trig)));
    CK(cudaMalloc(&g_tail_dev, sizeof(uint32_t)));
    CK(cudaMemset(g_tail_dev, 0, sizeof(uint32_t)));
    g_tail = 0;
    CK(cudaMalloc(&g_stop_dev, sizeof(uint32_t)));
    CK(cudaMemset(g_stop_dev, 0, sizeof(uint32_t)));
    CK(cudaMalloc(&g_rules_dev, sizeof(Rules)));

    // Seed the host rule table, honouring the combat-mode env var if present.
    gg_default_rules(&g_rules);
    if (const char *m = getenv("GG_COMBAT_MODE"))
        g_rules.combat_mode = (atoi(m) != 0) ? GG_MODE_BOUNDS : GG_MODE_REDERIVE;

    CK(cudaStreamCreateWithFlags(&g_copy,   cudaStreamNonBlocking));
    CK(cudaStreamCreateWithFlags(&g_kernel, cudaStreamNonBlocking));

    g_alloc = true;
    return 0;
}

static int launch_engine(void)
{
    if (g_up) return 0;
    if (alloc_buffers() != 0) return -1;

    // Push the final rule table to the device. After this, the rules are
    // device-resident and the host cannot edit them.
    CK(cudaMemcpy(g_rules_dev, &g_rules, sizeof(Rules), cudaMemcpyHostToDevice));

    uint32_t *d_val = nullptr; Ctl *d_ctl = nullptr;
    CK(cudaHostGetDevicePointer((void **)&d_val, g_val, 0));
    CK(cudaHostGetDevicePointer((void **)&d_ctl, g_ctl, 0));

    engine<<<1, GG_MAX_SLOTS, 0, g_kernel>>>(g_ring, g_tail_dev, g_stop_dev,
                                             d_val, g_rules_dev, d_ctl);
    CK(cudaGetLastError());
    g_up = true;
    return 0;
}

extern "C" int  gg_init(void)  { std::lock_guard<std::mutex> lk(g_lock); return launch_engine(); }
extern "C" void gg_start(void) { std::lock_guard<std::mutex> lk(g_lock); launch_engine(); }

extern "C" void gg_shutdown(void)
{
    std::lock_guard<std::mutex> lk(g_lock);
    if (!g_up) return;
    g_up = false;

    const uint32_t one = 1;
    CK_V(cudaMemcpy(g_stop_dev, &one, sizeof(uint32_t), cudaMemcpyHostToDevice));
    CK_V(cudaStreamSynchronize(g_kernel));

    CK_V(cudaStreamDestroy(g_copy));
    CK_V(cudaStreamDestroy(g_kernel));
    CK_V(cudaFree(g_ring));
    CK_V(cudaFree(g_tail_dev));
    CK_V(cudaFree(g_stop_dev));
    CK_V(cudaFree(g_rules_dev));
    CK_V(cudaFreeHost(g_stage));
    CK_V(cudaFreeHost(g_stail));
    CK_V(cudaFreeHost(g_ctl));
    CK_V(cudaFreeHost(g_val));
    g_ring = g_stage = nullptr; g_stail = nullptr;
    g_tail_dev = g_stop_dev = nullptr; g_rules_dev = nullptr;
    g_ctl = nullptr; g_val = nullptr;
    g_alloc = false;
    fflush(stdout); fflush(stderr);
}

extern "C" int gg_active(void) { return g_up ? 1 : 0; }

extern "C" void gg_set_combat_mode(int mode)
{
    std::lock_guard<std::mutex> lk(g_lock);
    if (g_up) { fprintf(stderr, "[gpuguard] combat mode is fixed after gg_start\n"); return; }
    if (alloc_buffers() != 0) return;
    g_rules.combat_mode = mode ? GG_MODE_BOUNDS : GG_MODE_REDERIVE;
}
extern "C" int gg_combat_mode(void) { return g_alloc ? (int)g_rules.combat_mode : -1; }

extern "C" int gg_alloc_slot(uint32_t initial)
{
    std::lock_guard<std::mutex> lk(g_lock);
    if (g_up) {
        fprintf(stderr, "[gpuguard] gg_alloc_slot after gg_start is not allowed\n");
        return -1;
    }
    if (alloc_buffers() != 0) return -1;
    if (g_next >= GG_MAX_SLOTS) {
        fprintf(stderr, "[gpuguard] out of slots (max %d)\n", GG_MAX_SLOTS);
        return -1;
    }
    const int slot = g_next++;
    g_val[slot] = initial;                 // baked into the shadow at launch
    return slot;
}

extern "C" volatile uint32_t *gg_slot_ptr(int slot)
{
    if (!g_val || slot < 0 || slot >= GG_MAX_SLOTS) return nullptr;
    return (volatile uint32_t *)&g_val[slot];
}

// THE ONLY WRITE-SIDE ENTRY POINT. Packs a Trig and enqueues it exactly as v1's
// gg_submit did -- entry copy, then same-stream doorbell bump. On Windows it is
// exported for the T1 injection demo.
GG_EXPORT uint64_t gg_trigger(int op, int32_t arg0, int32_t arg1, int32_t arg2, int32_t arg3)
{
    if (!g_up) return UINT64_MAX;
    std::lock_guard<std::mutex> lk(g_lock);

    volatile Ctl *c = ctl();
    const uint32_t seq = g_tail;
    const uint32_t k   = seq % RING_N;

    // Flow control: hold at most RING_N - GG_SLACK in flight so the host cannot lap
    // the ring. Corrupting head only stalls our own producer (self-DoS); the seq
    // check still gates every apply.
    while ((int32_t)(seq - c->head) >= (int32_t)(RING_N - GG_SLACK)) { /* drain */ }

    g_stage[k].seq  = seq;
    g_stage[k].op   = (uint32_t)op;
    g_stage[k].arg0 = arg0;
    g_stage[k].arg1 = arg1;
    g_stage[k].arg2 = arg2;
    g_stage[k].arg3 = arg3;

    if (cudaMemcpyAsync(&g_ring[k], &g_stage[k], sizeof(Trig),
                        cudaMemcpyHostToDevice, g_copy) != cudaSuccess)
        return UINT64_MAX;

    g_stail[k] = seq + 1;
    if (cudaMemcpyAsync(g_tail_dev, &g_stail[k], sizeof(uint32_t),
                        cudaMemcpyHostToDevice, g_copy) != cudaSuccess)
        return UINT64_MAX;

    g_tail = seq + 1;
    return (uint64_t)seq;
}

extern "C" int gg_wait(uint64_t seq)
{
    if (!g_up || seq == UINT64_MAX) return GG_PENDING;
    volatile Ctl *c = ctl();
    const uint32_t s = (uint32_t)seq;
    while ((int32_t)(c->head - s) <= 0) { /* ~10 us */ }
    return (int)c->verdict[s % RING_N];
}

extern "C" int      gg_tampered(void)    { return g_up ? (int)ctl()->tamper : 0; }
extern "C" int      gg_tamper_slot(void) { return g_up ? (int)ctl()->tamper_slot : -1; }
extern "C" uint32_t gg_applied(void)     { return g_up ? ctl()->applied  : 0; }
extern "C" uint32_t gg_rejected(void)    { return g_up ? ctl()->rejected : 0; }
extern "C" uint32_t gg_mismatch(void)    { return g_up ? ctl()->mismatch : 0; }

extern "C" const volatile void *gg_bell_addr(void)  { return g_up ? (const volatile void *)g_tail_dev : nullptr; }
extern "C" const void          *gg_ctl_addr(void)    { return g_up ? (const void *)g_ctl  : nullptr; }
extern "C" const void          *gg_ring_devptr(void) { return g_up ? (const void *)g_ring : nullptr; }
