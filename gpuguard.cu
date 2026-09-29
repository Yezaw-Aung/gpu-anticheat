// gpuguard.cu — the persistent engine and the C shim around it.  [VERSION 1]
//
//   The only file in this folder that nvcc has to see.
//
// *** VERSION 1: the raw WRITE CHANNEL. ***
// The host changes a protected value by submitting a value operation on its slot
// (gg_submit -> GG_OP_SET / GG_OP_ADD); the engine applies it to that slot's
// register shadow and mirrors it to the plaintext page. Simple and easy to scale.
// Its weakness is structural: the engine cannot tell a legitimate caller from
// injected code issuing the SAME submit, so an in-process attacker forges values
// at will. code/game/ (v2) removes the raw write path and keeps only GPU-owned
// rule triggers, which bounds that confused deputy.
//
// The plumbing below (ring in device memory, device-resident one-way doorbell,
// per-request seq check, GPU-register shadows) is identical to v2 -- only the
// engine's inner action differs (apply a value op here; evaluate a rule there).
//
//   1. Every slot's shadow is a KERNEL REGISTER. One thread per slot, so no
//      shadow ever touches memory an external process could reach.
//   2. The ring CONTENTS are in cudaMalloc'd device memory (EFAULT from another
//      process), so a request cannot be forged or edited from the host.
//   3. The DOORBELL (`tail`) is ALSO device memory; the host advances it only via
//      a cudaMemcpyAsync on the same stream as the ring-entry copy, so the bump
//      cannot overtake the entry it announces, and it cannot be replayed or frozen
//      from the host. The register `head` + per-request seq stay on as a net.

#include <cuda_runtime.h>
#include <cstdio>
#include <cstdlib>
#include <cstring>
#include <mutex>
#include "gpuguard.h"

#define CK(c) do { cudaError_t e_ = (c); if (e_ != cudaSuccess) {                \
    fprintf(stderr, "[gpuguard] CUDA %s:%d %s\n", __FILE__, __LINE__,            \
            cudaGetErrorString(e_)); fflush(stderr); return -1; } } while (0)

#define CK_V(c) do { cudaError_t e_ = (c); if (e_ != cudaSuccess) {              \
    fprintf(stderr, "[gpuguard] CUDA %s:%d %s\n", __FILE__, __LINE__,            \
            cudaGetErrorString(e_)); fflush(stderr); } } while (0)

#define RING_N 256
#define GG_SLACK 8      // flow-control headroom: cap in-flight at RING_N - GG_SLACK

struct Req { uint32_t slot, seq; int32_t arg; uint32_t op; };   // 16B, one load

// GPU-WRITE, HOST-READ only. Every engine INPUT (ring, doorbell, stop flag) lives
// in device memory the host cannot reach; the only host-visible state is these
// OUTPUTS. An attacker can corrupt an output to mislead the host's own reporting,
// but never to change an engine decision.
struct Ctl {
    uint32_t head, applied, rejected;             uint8_t p0[116];
    uint32_t tamper, tamper_slot;                 uint8_t p1[120];
};

// ---------------------------------------------------------------- the engine

__global__ void engine(const Req *ring, const uint32_t *d_tail,
                       const uint32_t *d_stop, uint32_t *val, volatile Ctl *c)
{
    const int s = threadIdx.x;
    volatile uint32_t *mine = (volatile uint32_t *)&val[s];

    // The shadow. A register for the lifetime of the process: not in host memory,
    // not in device memory, not addressable from anywhere off-GPU. SEEDED HERE
    // from the plaintext page, which the host filled with each slot's INITIAL
    // value before this kernel launched (gg_alloc_slot).
    uint32_t shadow = *mine;                      // GPU-init from the pre-launch page
    uint32_t head   = 0;

    // Only thread 0 reads the device-resident doorbell and stop flag, then
    // publishes them so every thread in the block agrees at each barrier.
    __shared__ uint32_t s_tail, s_stop;

    for (;;) {
        if (s == 0) {
            s_tail = *(const volatile uint32_t *)d_tail;   // device doorbell
            s_stop = *(const volatile uint32_t *)d_stop;   // device stop flag
        }
        __syncthreads();
        if (s_stop) break;                        // uniform across the block
        const uint32_t tail = s_tail;

        // Drain. Every thread walks every entry; only the owning thread acts.
        for (int i = 0; i < RING_N && (int32_t)(tail - head) > 0; i++) {
            const Req r = ring[head % RING_N];

            // head is a register the attacker cannot reach: the net if a doorbell
            // bump is ever seen before its entry.
            if (r.seq != head) {
                if (s == 0) c->rejected++;
            } else {
                // THE WRITE CHANNEL (v1). Apply the host's value operation to the
                // owning slot's register shadow, then mirror it to the plaintext
                // page. A legitimate caller and injected code reach this same line
                // through the same gg_submit -- the engine cannot distinguish them,
                // which is exactly the direct-forge / confused-deputy weakness v2
                // removes by having no raw write path at all.
                if ((uint32_t)s == r.slot) {
                    if      (r.op == GG_OP_SET) shadow = (uint32_t)r.arg;
                    else if (r.op == GG_OP_ADD) shadow = shadow + (uint32_t)r.arg;
                    *mine = shadow;               // heal the page to match the shadow
                }
                if (s == 0) c->applied++;
            }
            head++;
        }

        // Publish head only after every owner has applied.
        __syncthreads();
        if (s == 0) c->head = head;
        __threadfence_system();

        // Tamper poll: plaintext vs. register. Catches an EXTERNAL edit (a memory
        // editor, T0) -- it diverges from the shadow. It does NOT catch a forged
        // submit: that updated the shadow too, so there is no divergence. ~4 us.
        if (*mine != shadow) { c->tamper = 1; c->tamper_slot = s; }
    }
}

// ------------------------------------------------------------------ the shim

namespace {

bool          g_up      = false;   // engine launched
bool          g_alloc   = false;   // buffers allocated (may precede launch)
bool          g_failed  = false;
int           g_next    = 0;
uint32_t     *g_val     = nullptr;   // mapped pinned: the plaintext slots
Req          *g_ring    = nullptr;   // device only: request payloads
Req          *g_stage   = nullptr;   // pinned staging RING (RING_N requests)
uint32_t     *g_stail   = nullptr;   // pinned staging RING for doorbell bumps
uint32_t     *g_tail_dev= nullptr;   // device only: the doorbell (one-way)
uint32_t     *g_stop_dev= nullptr;   // device only: the stop flag (host cannot kill)
Ctl          *g_ctl     = nullptr;   // mapped pinned control block (GPU-write, host-read)
uint32_t      g_tail    = 0;         // host producer counter (NOT the GPU doorbell)
cudaStream_t  g_copy    = nullptr;
cudaStream_t  g_kernel  = nullptr;
std::mutex    g_lock;                // gg_submit is callable from any thread

volatile Ctl *ctl() { return (volatile Ctl *)g_ctl; }

} // namespace

// Allocate every buffer, but do NOT launch. Idempotent; lock held by the caller.
// Splitting allocation from launch enables GPU-init: gg_alloc_slot writes each
// slot's initial value into g_val, and the engine reads those into its shadows.
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

    CK(cudaHostAlloc(&g_stage, RING_N * sizeof(Req), cudaHostAllocDefault));
    memset(g_stage, 0, RING_N * sizeof(Req));
    CK(cudaHostAlloc(&g_stail, RING_N * sizeof(uint32_t), cudaHostAllocDefault));
    memset(g_stail, 0, RING_N * sizeof(uint32_t));

    CK(cudaMalloc(&g_ring, RING_N * sizeof(Req)));
    CK(cudaMemset(g_ring, 0, RING_N * sizeof(Req)));

    CK(cudaMalloc(&g_tail_dev, sizeof(uint32_t)));
    CK(cudaMemset(g_tail_dev, 0, sizeof(uint32_t)));
    g_tail = 0;

    CK(cudaMalloc(&g_stop_dev, sizeof(uint32_t)));
    CK(cudaMemset(g_stop_dev, 0, sizeof(uint32_t)));

    CK(cudaStreamCreateWithFlags(&g_copy,   cudaStreamNonBlocking));
    CK(cudaStreamCreateWithFlags(&g_kernel, cudaStreamNonBlocking));

    g_alloc = true;
    return 0;
}

// Launch the persistent engine. Idempotent; lock held. By now gg_alloc_slot has
// written every protected value's initial into g_val, so the engine seeds its
// shadows from that page.
static int launch_engine(void)
{
    if (g_up) return 0;
    if (alloc_buffers() != 0) return -1;

    uint32_t *d_val = nullptr; Ctl *d_ctl = nullptr;
    CK(cudaHostGetDevicePointer((void **)&d_val, g_val, 0));
    CK(cudaHostGetDevicePointer((void **)&d_ctl, g_ctl, 0));

    // Write-channel engine (v1). Never returns; occupies one SM for the life of
    // the process.
    engine<<<1, GG_MAX_SLOTS, 0, g_kernel>>>(g_ring, g_tail_dev, g_stop_dev, d_val, d_ctl);
    CK(cudaGetLastError());

    g_up = true;
    return 0;
}

extern "C" int gg_init(void)
{
    std::lock_guard<std::mutex> lk(g_lock);
    return launch_engine();
}

extern "C" void gg_start(void)
{
    std::lock_guard<std::mutex> lk(g_lock);
    launch_engine();
}

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
    CK_V(cudaFreeHost(g_stage));
    CK_V(cudaFreeHost(g_stail));
    CK_V(cudaFreeHost(g_ctl));
    CK_V(cudaFreeHost(g_val));
    g_ring = nullptr; g_stage = nullptr; g_stail = nullptr;
    g_tail_dev = nullptr; g_stop_dev = nullptr;
    g_ctl = nullptr; g_val = nullptr;
    g_alloc = false;
    fflush(stdout); fflush(stderr);
}

extern "C" int gg_active(void) { return g_up ? 1 : 0; }

extern "C" uint64_t gg_submit(int slot, int op, int32_t arg)
{
    if (!g_up || slot < 0 || slot >= GG_MAX_SLOTS) return UINT64_MAX;
    std::lock_guard<std::mutex> lk(g_lock);

    volatile Ctl *c = ctl();
    const uint32_t seq = g_tail;           // host producer counter, not a doorbell
    const uint32_t k   = seq % RING_N;

    // FLOW CONTROL. Hold at most RING_N - GG_SLACK requests in flight so the host
    // cannot lap the ring. Corrupting head only stalls our own producer (self-DoS);
    // the seq check still gates every apply.
    while ((int32_t)(seq - c->head) >= (int32_t)(RING_N - GG_SLACK)) { /* drain */ }

    g_stage[k].slot = (uint32_t)slot;
    g_stage[k].seq  = seq;
    g_stage[k].arg  = arg;
    g_stage[k].op   = (uint32_t)op;

    // Entry first...
    if (cudaMemcpyAsync(&g_ring[k], &g_stage[k], sizeof(Req),
                        cudaMemcpyHostToDevice, g_copy) != cudaSuccess)
        return UINT64_MAX;

    // ...then the doorbell bump, on the SAME stream (issue order does the ordering
    // the old mandatory sync used to). If ordering ever slipped, the engine reads a
    // stale seq and rejects -- never misapplies.
    g_stail[k] = seq + 1;
    if (cudaMemcpyAsync(g_tail_dev, &g_stail[k], sizeof(uint32_t),
                        cudaMemcpyHostToDevice, g_copy) != cudaSuccess)
        return UINT64_MAX;

    g_tail = seq + 1;
    return (uint64_t)seq;
}

extern "C" void gg_wait(uint64_t seq)
{
    if (!g_up || seq == UINT64_MAX) return;
    volatile Ctl *c = ctl();
    const uint32_t s = (uint32_t)seq;
    while ((int32_t)(c->head - s) <= 0) { /* ~10 us */ }
}

extern "C" int gg_alloc_slot(uint32_t initial)
{
    std::lock_guard<std::mutex> lk(g_lock);

    // Reserve during trusted startup, before launch, so the initial value is baked
    // into the shadow.
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
    g_val[slot] = initial;                 // GPU-init: baked into the shadow at launch
    return slot;
}

extern "C" volatile uint32_t *gg_slot_ptr(int slot)
{
    if (!g_val || slot < 0 || slot >= GG_MAX_SLOTS) return nullptr;
    return (volatile uint32_t *)&g_val[slot];
}

extern "C" int      gg_tampered(void)    { return g_up ? (int)ctl()->tamper : 0; }
extern "C" int      gg_tamper_slot(void) { return g_up ? (int)ctl()->tamper_slot : -1; }
extern "C" uint32_t gg_applied(void)     { return g_up ? ctl()->applied : 0; }
extern "C" uint32_t gg_rejected(void)    { return g_up ? ctl()->rejected : 0; }

extern "C" const volatile void *gg_bell_addr(void) { return g_up ? (const volatile void *)g_tail_dev : nullptr; }
extern "C" const void          *gg_ctl_addr(void)   { return g_up ? (const void *)g_ctl   : nullptr; }
extern "C" const void          *gg_stage_addr(void) { return g_up ? (const void *)g_stage : nullptr; }
extern "C" const void          *gg_ring_devptr(void){ return g_up ? (const void *)g_ring  : nullptr; }
