# v2 — the rule-reward engine

**Version 2** of the GPU anti-cheat: the GPU is the **authority over protected
game state**, not merely a monitor of it. The host cannot name a protected value;
it can only submit a **trigger** (`MOVE`, `SHOOT`, `ATTACK`, ...), and the
persistent GPU engine decides the result from its own authoritative prior state
plus a GPU-resident rule table. This removes v1's raw write channel — the
confused-deputy / direct-forge hole — and replaces it with a bounded one.

Read **`../DESIGN_v2.md`** for the threat model, the exact invariant, the honest
limitations, the related-work comparison, and the experiment plan. This README is
just how to build and drive it.

## The model in one line

> A protected value changes **only** through a transition the GPU-resident rules
> accept, evaluated against the GPU's **own** authoritative prior state. The CPU
> proposes; the GPU decides. Score is GPU-owned and rises only on a GPU-adjudicated
> kill.

## The core experiment: two combat modes

The CPU-heavy op (`ATTACK`) is verified in one of two modes, selected **before
launch** by `GG_COMBAT_MODE`:

| mode | `GG_COMBAT_MODE` | what the GPU does with CPU-claimed damage | T1 max-damage cheat |
|---|---|---|---|
| **REDERIVE** | `0` (default) | **ignores it**, recomputes from a GPU-owned roll + rules | **neutralised** (claim discarded; `mismatch` counter climbs) |
| **BOUNDS** | `1` | **trusts it** if `0 ≤ dmg ≤ max_dmg[weapon]` | **works, but bounded** (max-roll every swing) |

The measurable gap between these two modes — cheat score/DPS in BOUNDS vs
REDERIVE, at equal honest baseline — is the paper's central result. It shows GPU
authority prevents the T1 cheat **exactly** on operations the GPU fully re-derives,
and degrades to mere bounds-enforcement otherwise.

## Build & run

### Windows (CMake — the primary path)

Install **Visual Studio 2022** (Desktop development with C++) and the **CUDA
Toolkit**, then from a normal PowerShell or cmd prompt:

```bat
cmake -B build
cmake --build build --config Release
```

The first configure downloads and builds **raylib** (the graphics library) via
CMake FetchContent — nothing to install. This produces, side by side in
`build\Release\`:

- **`game_gfx.exe`** — the **graphical** GPU build (this is the one to run),
- `game.exe` — the terminal GPU build,
- `game_cpu.exe` / `game_gfx_cpu.exe` — CPU stubs (no CUDA; guard nothing),
- `cheat.dll` + `injector.exe` — the T1 injection demo (`attack/`).

Run it (pick a combat mode with the `GG_COMBAT_MODE` env var):

```bat
set GG_COMBAT_MODE=0   &  .\build\Release\game_gfx.exe   :: REDERIVE (GPU recomputes)
set GG_COMBAT_MODE=1   &  .\build\Release\game_gfx.exe   :: BOUNDS   (GPU trusts a cap)

:: T1 demo, in a second terminal (works against game_gfx.exe too):
.\build\Release\injector.exe game_gfx.exe .\build\Release\cheat.dll
```

**Graphical controls:** `WASD`/arrows move · **mouse** aims · `SPACE`/left-click
shoot a bullet · `F`/right-click melee the nearest monster · `C` **cheat** melee
(max claim) · `1`–`4` weapon · `R` reload · `E` use item · `X` try to forge score
(shows it's impossible) · `T` tamper the score page (watch the GPU heal it + alert).

Five monsters chase you (the GPU moves them and they deal contact damage). **Melee
only lands when you're within the blue range ring** — the GPU rejects out-of-range
swings from its own authoritative positions, so a T1 cheat can't hit across the
map. Bullets are CPU-simulated but damage is GPU-verified on hit. The HUD shows the
live GPU-owned state, the engine counters, and the `cpu-damage-mismatch` counter
that climbs in REDERIVE mode whenever the client lies about damage.

If CMake < 3.24 rejects `native`, pass your GPU's arch (86 = RTX 30xx Ampere):
`cmake -B build -DCMAKE_CUDA_ARCHITECTURES=86`. Disable the attack demo with
`-DBUILD_ATTACK_DEMO=OFF`, or the graphical build with `-DBUILD_GRAPHICS=OFF`.

> **TDR / watchdog.** The engine is a persistent kernel. Windows kills a kernel
> that runs longer than ~2 s unless the GPU watchdog (TDR) is disabled — set
> `HKLM\SYSTEM\CurrentControlSet\Control\GraphicsDrivers\TdrLevel = 0` and reboot,
> or run the GPU without a display attached. Without this the engine is torn down.

CMake configures even on a machine **without** the CUDA Toolkit — it warns and
builds only `game_cpu` (the stub), so you can develop game logic anywhere.

### Linux (CMake or Make)

```bash
cmake -B build && cmake --build build      # -> build/game  (and build/game_cpu)
# or the Makefile:
make                 # GPU build (needs nvcc) -> ./game
make cpu             # CPU stub (no CUDA)      -> ./game_cpu   (UNPROTECTED banner)
make ARCH=sm_86      # override arch if -arch=native is unavailable

GG_COMBAT_MODE=0 ./game      # REDERIVE
GG_COMBAT_MODE=1 ./game      # BOUNDS
```

The CPU stub runs the **identical** rule core (`game_rules.cuh`), so the economy
behaves the same on a laptop — it just protects nothing (no GPU authority). Use it
to develop game logic; use the GPU build for any security claim or measurement.

## Commands

```
move <l|r|u|d>   shoot   reload   tick   item        # GPU-friendly ops
attack [weapon]                                       # CPU-heavy op (honest)
cheat_score                                           # show the v1 forge is inexpressible
cheat_attack [n]                                      # T1: max claimed damage x n
tamper                                                # T0: external page edit (healed+flagged)
score | status | addrs | bench [n] | help | quit
```

## Files

- `game_gfx.cpp` — the **graphical** frontend (raylib). Same security model as the
  terminal game; only the presentation differs — it drives the same triggers and
  reads the same GPU-owned slots.
- `game.cpp` — the terminal combat game (host side: triggers only, read-only state).
- `game_rules.cuh` — **the rule core**: slot layout (player + per-monster blocks),
  trigger vocabulary (incl. `ATTACK` melee range-gate, `BULLET_HIT`, `MONSTER_STEP`),
  the rule table, and `gg_apply_rule()`. Compiled by **both** nvcc (engine) and the
  host (stub), so the enforced rule and the simulated rule are the same source.
- `game_common.h` — shared host helpers: canonical slot allocation order
  (`gg_setup_slots`), slot reader (`gg_rd`), and the CPU damage-proposal function.
- `gpuguard.h` — the C ABI: `gg_trigger` (the only write-side entry), no raw write.
- `gpuguard.cu` — the persistent engine (shared-memory shadows, device ring +
  one-way doorbell, per-request verdict) and the host shim.
- `gpuguard_stub.cpp` — the no-GPU stub; runs the same rules, guards nothing.
- `protected.h` — the **read-only** game-facing view (header-only; no write path).
- `attack/` — the T1 injection demo and the honest contrast with v1.

## Relationship to v1

The repository root is **v1**, the raw write channel. Its `forge` command and
`attack/` injection demo show the confused-deputy hole. Run the same class of
attack here to see it bounded (REDERIVE) or at least shaped (BOUNDS), and the
direct score-forge gone entirely. Keeping both versions is deliberate: the paper's
argument is the **delta** between them.
