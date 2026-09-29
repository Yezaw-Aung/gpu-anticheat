# game-v1 — the write-channel version

This is **version 1** of the terminal coin game: the original **write-channel**
design, kept alongside `../game/` (version 2, the rule-reward engine) so the two
can be compared directly in the paper.

## The model

Money is held in `Protected` counters (`pennies`, `dollars`). The game changes
them with ordinary arithmetic:

```cpp
pennies += 10;     // -> gg_submit(slot, GG_OP_ADD, 10)
pennies -= 100;    // -> gg_submit(slot, GG_OP_ADD, -100)
dollars += 1;      // -> gg_submit(slot, GG_OP_ADD, 1)
```

Each mutation forwards to **`gg_submit`**, the write channel. The persistent GPU
engine applies the value to that slot's **register shadow** and mirrors it to the
plaintext page. Reads are a plain load from the page.

**Why v1 exists:** it is simple and scales to any value or type with no rule
authoring — this is the "easy to implement, static and dynamic" property. The cost
is the hole below.

## What it defends — and the hole it leaves

| Command | Attacker | Result |
|---|---|---|
| `tamper` | **T0** external memory editor pokes the plaintext page | **DETECTED** — diverges from the register shadow, alert fires |
| `forge`  | **T1** injected in-process code calls the same `gg_submit` | **APPLIED, no alert** — the engine cannot tell it from a legitimate write |

`forge` is v1's central weakness: the **confused deputy / direct forge**. Because
the write channel accepts a *value*, any code in the process can submit any value.
Run `forge` here, then run the same command in `../game/` (v2) — there it is
**refused**, because v2 has no raw write path (the engine is trigger-only and the
GPU owns every reward). That contrast is the point of keeping both versions.

## Build & run

```bash
make          # GPU build (needs nvcc) -> ./game
make cpu      # CPU stub (no CUDA)     -> ./game_cpu   (UNPROTECTED banner)
```

On the GPU box, override the arch if `native` is unavailable, e.g. `make ARCH=sm_86`.

## Files

- `game.cpp` — the terminal game (host-side economy: play/exchange).
- `protected.h` / `protected.cpp` — `Protected` **with** mutating operators that
  forward to the write channel (removed in v2).
- `gpuguard.h` — the C interface; `gg_submit(SET/ADD)` is the live write path.
- `gpuguard.cu` — the persistent engine (applies SET/ADD to register shadows) and
  the host shim (device-resident ring + one-way doorbell, seq-checked).
- `gpuguard_stub.cpp` — no-GPU stub; applies SET/ADD on plain memory so the logic
  runs on a laptop.

There is **no `game_rules.cuh`** in v1 — rules are a v2 concept.
# gpu-anticheat
