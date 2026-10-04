# v2/attack — T1 code-injection demo for v2 (Windows)

Demonstrates how far v2 **bounds** the in-process attacker that v1 could not stop.
Once code runs inside the game process it can call the game's own `gg_trigger`
(same CUDA context, ring, doorbell) — but, unlike v1's `gg_submit`, the worst it
can do is submit **rule-valid triggers**. There is no raw write path, so the v1
forge (`set score = 999999`) **cannot be expressed at all**.

**Authorized use only.** Targets *your own* `game.exe` on *your own* machine.

- `cheat.cpp` → `cheat.dll` — resolves `gg_trigger` and spams `ATTACK` with max
  claimed damage.
- `injector.cpp` → `injector.exe` — the same `CreateRemoteThread`+`LoadLibrary`
  injector as v1 (code injection is not what changed; the defence is).

## Build (x64 Native Tools Command Prompt for VS 2022)

```
cl /LD cheat.cpp user32.lib
cl injector.cpp
```

## Run

1. Start the game in a chosen combat mode (see top-level `v2/README.md`):
   ```
   set GG_COMBAT_MODE=0   &  game.exe      :: REDERIVE (GPU recomputes)
   set GG_COMBAT_MODE=1   &  game.exe      :: BOUNDS   (GPU trusts a capped claim)
   ```
2. In a second terminal, from this folder: `injector.exe game.exe cheat.dll`
3. Back in the game: `status`

## The contrast (the paper's point)

| action | mechanism | v1 result | v2 result |
|---|---|---|---|
| set score directly | `gg_submit(SET,999999)` / no such call | **forged, undetected** | **impossible** — no such trigger |
| external page edit | poke the value page | detected | detected **+ auto-healed** |
| inject + max damage | `gg_trigger(ATTACK, claim=90)` | n/a | **REDERIVE: neutralised** (claim ignored, `mismatch` climbs) / **BOUNDS: works but bounded** (max-damage swings only) |

The honest finding: v2 does **not** make the injected attacker disappear. It
reduces its power from *"any value"* to *"any sequence of rule-valid transitions,"*
and — in REDERIVE mode — strips its advantage on the verified op while leaving a
`mismatch` signature. In BOUNDS mode the attacker still max-rolls every swing.
That mode gap is what the evaluation measures; see `../DESIGN_v2.md`.

## Notes

- `gg_trigger` is exported from `game.exe` for the demo (as v1 exported
  `gg_submit`). A real cheat would locate it by signature scanning; the capability
  is identical.
- The demo cites a stale `expected_monster_hp`, so the GPU's CAS/ordering guard
  rejects those swings — showing that even a blind in-process attacker is further
  constrained by the stale-request guard. A cheat that first reads the mirror page
  (`gg_slot_ptr(SLOT_MON_HP)`) would cite the live value; do that to exercise the
  accepted-swing path.
