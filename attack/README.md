# attack/ — T1 code-injection demo for v1 (Windows)

Demonstrates v1's structural weakness (the confused deputy): once code runs
**inside** the game process, it can call the game's own `gg_submit` — using the
game's CUDA context, ring, and doorbell — so the GPU engine cannot tell it from
the game. The value is forged with **no tamper alert**. This is the realistic,
external version of the game's built-in `forge` command.

**Authorized use only.** This targets *your own* `game.exe` on *your own* machine
to demonstrate the vulnerability the paper studies.

- `cheat.cpp` → `cheat.dll` — the injected code; resolves `gg_submit` and forges
  pennies (slot 0) to 999999.
- `injector.cpp` → `injector.exe` — standard `CreateRemoteThread`+`LoadLibrary`
  injector; finds `game.exe` by name and loads `cheat.dll` into it.

## Build (x64 Native Tools Command Prompt for VS 2022)

```
cl /LD cheat.cpp user32.lib
cl injector.cpp
```

## Run

1. Build and start the game (with TDR disabled — see the top-level README):
   ```
   game.exe
   ```
2. In a second terminal, from the `attack` folder:
   ```
   injector.exe game.exe cheat.dll
   ```
   A message box confirms the injected code called `gg_submit`.
3. Back in the game:
   ```
   score      ->  pennies = 999999
   status     ->  alert = 0     (forged, undetected)
   ```

## The contrast (this is the paper's point)

| action | mechanism | result |
|---|---|---|
| `tamper` (in-game) | external edit of the value page | `alert = 1` — **detected** (the shadow catches it) |
| **inject cheat.dll** | in-process code calls `gg_submit` | `alert = 0` — **forged, undetected** |

Editing memory is caught; **calling the write channel from injected code is not** —
because the GPU cannot distinguish the injected caller from the game. That is the
confused deputy, and it is what the rule-reward design (v2) bounds and what the
range/rate cap would otherwise limit. It requires **in-process execution** (T1);
an external editor (T0) cannot reach the device-resident ring to do this.

## Notes

- For this demo, `gg_submit` is exported from `game.exe` (`GG_EXPORT` in
  `gpuguard.cu`) so `cheat.dll` can resolve it with `GetProcAddress`. A real cheat
  would find the address by **signature scanning**; the capability is identical.
- DLL injection is one delivery route for in-process code among several
  (manual mapping, thread hijacking, the game's own mod/script API). The forge
  needs *in-process execution*, not DLL injection specifically.
