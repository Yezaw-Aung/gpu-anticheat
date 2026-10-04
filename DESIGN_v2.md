# GPU-as-Authority for Game-State Integrity — v2 Design & Analysis

A research prototype in which a **GPU holds the authoritative copy of
security-sensitive game state** and admits changes only through rules that live in
GPU-resident memory. The CPU proposes transitions; the GPU decides them. This
document is the design, the threat analysis, and the experiment plan. It is
deliberately critical: it states the one invariant the architecture actually
enforces, and it is explicit about everything that invariant does **not** buy.

> **Scope note.** This is a systems-security *measurement* study, not a
> cheat-prevention product. The defensible contribution is a precise
> characterisation of how far "GPU-as-authority" bounds an in-process adversary on
> commodity NVIDIA hardware — and where it provably fails. Claims of general cheat
> prevention are out of scope and would be false.

---

## 0. The two versions, and why both exist

| | **v1 (repo root)** — write channel | **v2 (`v2/`)** — rule-reward engine |
|---|---|---|
| host → GPU vocabulary | `SET(slot,v)`, `ADD(slot,d)` | `TRIGGER(MOVE/SHOOT/ATTACK/...)` |
| can the host name a value? | **yes** | **no** |
| T1 (in-process) forge | **any value, no alert** (confused deputy) | **bounded to rule-valid transitions**; direct score-forge inexpressible |
| T0 (external) edit | detected | detected **+ auto-healed** |
| transport | device ring + one-way doorbell + seq | **identical** |

v2 changes exactly one thing in the trusted path — the engine's inner action, from
"apply a value op" to "evaluate a rule" — and that single change is the entire
research claim. Keeping v1 alongside lets the evaluation measure the **delta**
rather than assert it.

---

## 1. Architecture: CPU ↔ CUDA ↔ GPU-authoritative state

```
   host (game.exe)                                   GPU (persistent engine, 1 block)
   ───────────────                                   ────────────────────────────────
   gg_trigger(op,args) ─┐                            ┌────────────────────────────────┐
     stage Trig         │  cudaMemcpyAsync (stream)  │ shadow[SLOT_COUNT]  (shared mem)│  ← authority
     bump doorbell  ────┼──────────────────────────▶│ Rules               (device mem)│  ← rules
                        │  (entry first, then tail,  │ head                (register)  │  ← seq net
   read mirror page ◀───┤   same stream = ordered)   │                                 │
   gg_wait(seq)→verdict │                            │ for each new Trig:              │
                        │  mirror page (mapped) ◀────┤   seq==head? gg_apply_rule(...) │
   tamper poll ◀────────┘  Ctl outputs (mapped) ◀────┤   heal mirror; poll for edits   │
                                                     └────────────────────────────────┘
```

**Trust boundary.** Everything the engine *reads to make a decision* is off-limits
to the host:

- **shadow state** — on-chip **shared memory** (v1 used registers; v2 uses shared
  memory so one rule can span several slots). No off-GPU address exists for it;
  neither T0 (another process) nor T1 (host code in this process) can read or write
  it directly.
- **rule table** — `cudaMalloc`'d **device memory**, seeded once at launch, never
  host-writable afterward. `EFAULT` from another process.
- **request ring + doorbell + stop flag** — device memory; the doorbell is advanced
  only by a same-stream `cudaMemcpyAsync` after the entry copy, so a bump can never
  overtake the entry it announces.

The only host-visible state is **outputs**: a mirror page (so the game can read
values cheaply) and a `Ctl` block (counters, verdicts, tamper flag). An attacker
can corrupt an output to mislead the host's own reporting, but **never to change an
engine decision** — the engine never reads them back.

**Why one block, one engine thread for rules.** Requests are strictly ordered and a
rule may touch several slots (e.g. `USE_ITEM` spends score *and* raises HP;
`ATTACK` reads monster HP/armor and writes HP/score/RNG). Serial evaluation by
thread 0 over a shared-memory shadow is the simplest correct shape; the tamper poll
is still parallel across threads. This caps throughput at one SM and one rule at a
time — acceptable for event-rate state (see §7, §14), not for per-frame bulk data.

---

## 2. Game design: GPU-friendly vs CPU-heavy operations

A minimal top-down combat game with real protected state (`v2/game.cpp`,
`v2/game_rules.cuh`). Protected slots: `HP, AMMO, POSX, POSY, WEAPON_CD, MON_HP,
MON_ARMOR, BUFF, RNG, SCORE`.

**GPU-friendly ops** — cheap to *fully* validate on the GPU, so the GPU is the true
authority:
- `MOVE(dir)` — bounds-checked position update.
- `SHOOT` — requires `ammo>0` and `cooldown==0`; decrements ammo, sets cooldown.
- `RELOAD`, `TICK` (cooldown--/RNG step), `USE_ITEM` (transactional: score→HP, capped).

**CPU-heavy op** — `ATTACK`. The CPU does the expensive part (collision, buffs,
status, a damage roll) and proposes a result; the GPU verifies it. This is the
operation on which the whole security story turns (§6).

The headline invariant is cleanest on **SCORE**: there is *no* trigger that writes
it. It rises only inside the engine when an `ATTACK` drives monster HP to zero. No
in-process code can grant score without the GPU first adjudicating a kill from
authoritative monster HP.

**Spatial authority (multi-monster build).** The prototype now holds `GG_NMON`
monsters, each with GPU-authoritative HP, armor, **and position**. Because the GPU
owns positions, it enforces a *spatial* rule the CPU is not trusted to check:
melee `ATTACK` is rejected unless the player is within melee range of the target
monster, computed from authoritative positions. A T1 attacker therefore cannot hit
a monster it is not actually near — the range gate is part of the invariant, not a
client-side courtesy. Monsters chase the player and deal contact damage via the
GPU's `MONSTER_STEP` rule. Two honest limits this introduces, both already in the
threat model: (a) the *tempo* of `MONSTER_STEP` is CPU-driven, so a T1 attacker can
slow or freeze monster movement — the rate-cheat class of §11 (the GPU owns where
monsters are, not when they move); (b) a bullet's *trajectory* is CPU-computed, so
`BULLET_HIT` has the GPU verify the target's liveness, bullet-range bound, and
damage, but trust that a bullet geometrically reached it. Melee carries no such
trajectory trust — its range is fully GPU-checked.

---

## 3. The GPU rule system

A rule is a pure function of `(authoritative shadow, rule table, request)` →
`(accept?, new shadow)`. It is defined once, in `gg_apply_rule()` in
`game_rules.cuh`, and compiled by **both** nvcc (the engine) and the host compiler
(the stub). This anti-drift property matters for a paper: the rule the GPU enforces
and the rule the reference/stub simulates are literally the same source lines.

- **Rule *logic*** is code (a `switch` over trigger opcodes).
- **Rule *parameters*** (`move_step`, bounds, `weapon_cooldown`, `dmg_base/var/max`,
  `kill_reward`, ...) are **data** in the device-resident `Rules` table, seeded at
  launch and immutable thereafter.

Logic-in-code vs fully data-driven rules is a spectrum; parameters-in-device-memory
is sufficient for the invariant (the host cannot edit them at runtime). A
fully data-driven bytecode interpreter on the GPU is possible future work and would
strengthen the "rules are data in protected memory" framing, at an interpretation
cost worth measuring.

---

## 4. `gg_trigger()` design

```c
uint64_t gg_trigger(int op, int32_t a0, int32_t a1, int32_t a2, int32_t a3);
int      gg_wait(uint64_t seq);   // -> GG_APPLIED | GG_REJECTED
```

Replaces v1's `gg_submit(slot, SET/ADD, v)`. The ABI carries **no slot and no
value** for the write side — only an opcode and generic args whose meaning is
per-op (e.g. `ATTACK`: `a0=monster slot, a1=expected HP (CAS), a2=claimed damage,
a3=weapon`). The host **cannot express** "set value X". Transport is v1's, verbatim:
stage → entry copy → same-stream doorbell bump → return producer seq. A per-request
**verdict** channel (`Ctl.verdict[seq % RING_N]`, GPU-write/host-read) lets
`gg_wait` report accept/reject, which v1 lacked and the heavy path needs.

On Windows `gg_trigger` is exported for the T1 demo, exactly as v1 exported
`gg_submit`. That is a convenience, not a weakness: a real cheat finds the symbol by
signature scan. The point is that calling it yields only rule-valid transitions.

---

## 5. Protected-state representation in GPU memory

- **Authority:** `__shared__ uint32_t shadow[SLOT_COUNT]`, seeded at launch from the
  pre-launch mirror page (which `gg_alloc_slot` filled with each slot's initial).
  On-chip; no host-reachable address.
- **Mirror page:** mapped pinned host memory, **read-only by contract** for the
  game. The engine heals it to the shadow each iteration so reads are a plain load
  (~1 ns). A T0 edit of the mirror diverges from the shadow, is flagged, and is
  **overwritten** on the next poll — the value never actually changes.
- **Durability:** state is process-lifetime (registers/shared memory vanish on
  kernel exit). Persistence across sessions would require sealing the shadow to
  device memory under a GPU-held key — out of scope, and a genuine limitation for
  any real deployment.

---

## 6. CPU-generated results verified by the GPU — **the core problem**

This is where the architecture is won or lost, and where most naive versions of
this idea quietly fail. For `ATTACK`, "verify the CPU's damage" has three possible
meanings, and the prototype implements the two that matter as a switchable mode
(`GG_COMBAT_MODE`), so the evaluation can measure the difference:

1. **REDERIVE (true authority).** The GPU recomputes damage from a **GPU-owned
   roll** (counter-based RNG whose state is a protected slot the CPU can neither
   read nor set) and the rule table, reading buffs/armor from authoritative slots.
   The CPU's claimed damage is **ignored** (only logged as a `mismatch` when it
   disagrees). A T1 attacker cannot inflate damage; its only remaining freedom is
   choosing a legal weapon. **But** the CPU helper saved nothing — the GPU did the
   combat math. The "CPU as helper" story is weakest here and the security story is
   strongest.
2. **BOUNDS (the trap).** The GPU trusts the claim iff `0 ≤ dmg ≤ max_dmg[weapon]`.
   This is cheap and keeps the CPU as the real computer — but a T1 attacker submits
   `max_dmg` on **every** swing. Each transition is individually "legal"; the cheat
   nonetheless succeeds. This is precisely the **detecting-vs-preventing** trap the
   research brief warns about, made concrete and measurable.
3. **MAC/commitment (dead end under T1).** Trust a CPU result carrying an
   unforgeable tag. Requires a secret the attacker can't read — but T1 has
   arbitrary in-process execution, so any in-process key is readable. The only way
   out is a key that never leaves the GPU *and* a tag the GPU computes — which is
   REDERIVE again. Documented, not implemented.

**The measured finding (smoke test, CPU stub, identical rule core):** 20 max-claim
cheat swings with weapon 3 yielded **score 700 in REDERIVE** (equal to an honest
player's expected rolls; `mismatch=20`) vs **score 1000 in BOUNDS** (`mismatch=0`).
The ~43% advantage exists in BOUNDS and vanishes in REDERIVE. On the GPU the
numbers will match (same source), modulo the GPU RNG stream.

> **Thesis, stated honestly:** *GPU-as-authority prevents the T1 damage cheat
> exactly on operations the GPU can cheaply re-derive; on operations it can only
> bounds-check, it reduces the attacker to "always optimal within the rules" and
> leaves a `mismatch` signature, but does not prevent the cheat.* Classifying every
> operation on the axis **(GPU re-derivation cost) × (attacker gain if only
> bounds-checked)** is the paper's organising contribution.

---

## 7. Ordering, races, sequence numbers, stale requests

- **Seq net.** Each request carries `seq`; the engine's `head` is a **register** the
  attacker cannot reach. `seq != head` → reject. A doorbell bump seen before its
  entry therefore cannot misapply.
- **Same-stream ordering.** Entry copy and doorbell bump are issued on one stream;
  issue order is the ordering. If it ever slipped, the engine reads a stale seq and
  rejects — never misapplies.
- **CAS / stale guard (heavy path).** `ATTACK` must cite `expected_monster_hp`; the
  engine rejects if it ≠ the authoritative value. This defeats stale candidates and
  the `legitimate → malicious → legitimate` transient race on the heavy path: a
  request computed against an old value is rejected, not applied.
- **Flow control.** In-flight capped at `RING_N − SLACK` so the host cannot lap the
  ring; corrupting `head` only self-DoSes the producer.
- **Monotonic apply.** Rejections leave the shadow untouched on the reject-before-
  mutate paths. (`ATTACK` checks CAS and weapon bounds before any write.)

---

## 8. TOCTOU around the CPU staging buffer

Reframe the problem by threat:

- **Simple ops.** The only host-visible source is the staging slot, snapshotted into
  device memory by a single async copy. The T0 race window is the gap between filling
  the staging slot and the copy — sub-microsecond, matching v1's measured ≈169 wins
  per 1e6 frames. **Hardening to measure:** copy from a fresh stack `Trig` with no
  durable per-frame pinned slot a T0 editor can camp on, and re-measure the rate.
- **Heavy ops.** The real TOCTOU is the **CPU candidate buffer** (the damage the CPU
  computed before submitting). For T1 this is not a race to win — the attacker
  **owns** that buffer; it authors the candidate. So "shrink the TOCTOU window" is
  the wrong frame for the heavy path under T1: there is no window, there is an
  author. REDERIVE mode is the only structural answer (ignore the candidate).

**Do not claim** that hiding the staging address helps. It does not; the design
rests on integrity (CAS + rules + device-resident authority), not secrecy.

---

## 9. What a T0 (external) attacker can and cannot do

| | T0 |
|---|---|
| edit the mirror page | **yes → detected within one poll (~µs) and healed** |
| reach shadow / rules / ring / doorbell | **no** (on-chip or device memory; `EFAULT`) |
| call `gg_trigger` | **no** (no CUDA context in its process) |
| set a protected value | **no** (no write path; edits are reverted) |
| win a staging-race transient edit | rarely (~1.7e-4/frame, to be re-measured) |

T0 is comprehensively handled: its only lever is the mirror page, which is an output
the engine continuously overwrites.

---

## 10. What a T1 (in-process) attacker can and cannot do

| | T1 |
|---|---|
| set a protected value directly | **no** (no raw write path; v1's forge is inexpressible) |
| call `gg_trigger` | **yes** — indistinguishable from the game |
| forge an arbitrary value via triggers | **no** — only rule-valid transitions |
| grant itself score | **no** — score is GPU-owned (kill-only) |
| inflate `ATTACK` damage | **REDERIVE: no** (claim ignored) / **BOUNDS: yes, up to the cap** |
| rate/optimal-play cheat (spam `TICK` to clear cooldown; fire at inhuman rate) | **yes** — see §11 |
| read on-chip shadow or device rules | **no** (no mapping) |

T1 is the research frontier. v2 reduces it from *"any value"* to *"any sequence of
individually-legal transitions,"* and REDERIVE further strips its advantage on the
verified op. It does **not** eliminate T1 cheating.

---

## 11. Can the GPU distinguish a legitimate request from a malicious in-process one?

**No — and this is a theorem, not a bug.** A legitimate request and an injected one
reach the engine through the same ring, in the same CUDA context, with no
distinguishing bit. The GPU authenticates the **transition**, never the
**principal**. Three consequences the paper must state plainly:

1. **Rate cheats are out of reach.** Bounding "SHOOT rate" needs a trusted wall
   clock per player action. The CPU controls submission timing, so any CPU-supplied
   time is attacker-controlled. The GPU's own `globaltimer`/`clock64()` is monotonic
   and not CPU-forgeable, but it measures GPU time and cannot tell lag from a batched
   cheat. `TICK`-spam to clear cooldown is a concrete, deliberately-left-in example.
   **Claim nothing about rate cheats.**
2. **Semantic cheats within the rules** (always-optimal legal play, BOUNDS
   max-damage) remain. The `mismatch` counter *detects* a lying client in REDERIVE
   but is a heuristic, not a prevention.
3. **Caller authentication is exactly what a TEE/enclave provides and this design
   does not.** That is the honest reason a TEE comparison belongs in the paper (§12).

---

## 12. Exact security guarantees the system can claim

**Enforceable (hold vs both T0 and T1 unless noted):**

- **G1 (reachability bound).** No protected value ever takes a value outside the set
  reachable by some sequence of rule-valid transitions from its seed, evaluated
  against the GPU's own prior state.
- **G2 (external-edit detection + healing).** Any edit of the host-visible mirror
  page is detected within one engine poll and reverted to the authoritative value.
  *(T0, and T1 if it bothers to edit the page.)*
- **G3 (no direct naming).** The host cannot name a protected value; it can only
  propose a transition the GPU adjudicates against its own prior state. v1's
  direct-forge is not expressible.
- **G4 (GPU-owned reward).** `SCORE` changes only inside a GPU-adjudicated kill;
  there is no trigger that writes it.
- **G5 (REDERIVE authority).** For re-derived operations, the committed result is a
  function of GPU-held state and GPU-owned randomness only; CPU-claimed values do
  not affect it. *(Holds vs T1.)*

**Must NOT claim:** prevention of cheating in general; authentication of the caller
(§11); prevention of rate / optimal-play cheats; protection of any bounds-checked
operation beyond its bound; confidentiality of the mirror page; persistence of state
across process death.

---

## 13. Limitations and remaining attack surface

1. **Caller anonymity (§11)** — the fundamental one.
2. **BOUNDS-mode semantic cheats (§6)** — legal max-rolls.
3. **Rate cheats / no trusted clock (§11)**.
4. **Single-SM, serial rule throughput (§1)** — fine for event-rate state; a
   per-frame or high-fan-out workload needs a different kernel shape (and loses the
   simple cross-slot story).
5. **Output-corruption of `Ctl`** — misleads host reporting only, never a decision;
   but a deployment must not drive logic off these outputs.
6. **Persistence / sealing (§5)** — state dies with the process.
7. **TDR / watchdog** — a persistent kernel needs the OS GPU watchdog disabled, a
   deployment nuisance and an availability/DoS consideration.
8. **DoS** — an attacker can spam rejects or stall its own producer; it degrades
   itself, but a shared engine across many objects needs isolation thought.
9. **Rules-as-code, not data (§3)** — weakens "rules live in protected memory" from
   literal to parameter-level.
10. **Side channels** — timing of accept/reject may leak rule thresholds; unmeasured.

---

## 14. Experimental plan

**Platform.** Consumer NVIDIA (e.g. RTX 3060), CUDA ≥ 11.5, TDR disabled. Build the
GPU target (`make`); the CPU stub is for logic/reference only and must never back a
security number. **This environment has no GPU; all GPU numbers below must be
produced on the RTX box.**

### 14.1 Security efficacy (the headline)
For each mode ∈ {REDERIVE, BOUNDS} and each attacker ∈ {honest baseline, T0 editor,
T0 staging-racer, T1 `gg_trigger` caller, T1 injected DLL}:
- **successful-cheat rate** — fraction of attempts that changed authoritative state
  in the attacker's favour. Expected: T0 ≈ 0 (G2); T1 direct-forge = 0 (G3/G4); T1
  `ATTACK` cheat = 0 in REDERIVE, > 0 in BOUNDS.
- **detection rate** — T0 edits flagged (expect ~100%); REDERIVE `mismatch` rate
  under a lying client (expect 100% of lies logged).
- **cheat advantage** — cheater score/DPS ÷ honest score/DPS, per mode. The
  REDERIVE-vs-BOUNDS gap is *the* result (smoke test: 1.0 vs ~1.4).

### 14.2 The re-derivation × attacker-gain map
Classify each op (MOVE/SHOOT/RELOAD/USE_ITEM/ATTACK) by GPU re-derivation cost
(µs/op) and attacker gain if only bounds-checked. Show the clustering: cheap-to-
verify ops (movement/ammo/economy) vs verify=recompute ops (combat).

### 14.3 Performance
- **latency** — `gg_wait` round-trip per op (`bench` command); v1 reported ~7.3 µs.
- **frame-time impact** — triggers/frame × latency vs a 16.6 ms budget.
- **throughput** — max applied triggers/s (one serial engine).
- **overheads** — CPU (submit path), GPU memory (`O(SLOT_COUNT + RING_N)`, tiny),
  one occupied SM.
- **staging-window duration** — measured gap; T0 race success before/after the §8
  stack-`Trig` hardening.

### 14.4 Scaling
Sweep **#protected objects** (SLOT_COUNT), **#rules**, and **rule complexity**
(REDERIVE arithmetic depth, or a data-driven interpreter if added). Report
latency/throughput vs each. Expectation: flat until the serial engine saturates,
then linear — quantifying the single-SM ceiling.

### 14.5 Ablations
- v1 vs v2 on the identical T1 injection (`attack/`): forge succeeds in v1, is
  bounded/neutralised in v2.
- REDERIVE vs BOUNDS at equal honest baseline.
- Staging hardening on/off (§8).

---

## 15. Related work and positioning

- **Ritsumeikan 2025, "Detecting Memory Editing Cheats by Validating Host Memory
  Integrity from GPU."** Pure **detection**: the GPU polls host memory and flags
  divergence. v2's tamper poll (and all of v1) is the same class. **v2's novelty is
  the control-flow inversion** — the GPU is *in the write path as the authority*,
  not merely observing it. Be scrupulous: the advantage over pure detection exists
  only for operations v2 can re-derive, and **collapses to parity** on bounds-checked
  ops. Fair claim: *strictly stronger on fully-re-derivable operations; equivalent
  elsewhere.*
- **CPU-side validation.** Same rule logic, but authority + rules live in attackable
  memory → T1 edits the validator. v2's edge is that authority and rules sit where
  T1 cannot edit them (on-chip / device memory). A microbenchmark against an
  in-process CPU validator makes this concrete.
- **Polling/comparison GPU integrity.** Detection only; same positioning as the
  Ritsumeikan line.
- **TEE / SGX / VBS enclaves.** These *do* authenticate code and attest — exactly
  what v2 lacks (§11). Honest comparison: a TEE solves the principal-authentication
  problem v2 cannot; v2 targets commodity gaming GPUs with no enclave, trading
  caller authentication away. That trade is the reason the work exists.

**The distinction to foreground throughout:** *detecting that memory was modified*
(prior work, and v2's T0 story) vs *preventing unauthorised state transitions by
making the GPU the authority* (v2's T1 story) — and the explicit demonstration that
the second does **not** automatically solve T1 (BOUNDS mode proves it).

---

## 16. Is this publishable? A critical verdict

**Yes, as a systems-security measurement paper,** if framed as "how far does
GPU-as-authority bound an in-process adversary, and where does it provably fail,"
**not** as a cheat-prevention system.

- **Genuinely novel:** the control-flow inversion (GPU as authority, not monitor) on
  commodity hardware; G1/G3/G4/G5; and — most publishable — the quantified
  re-derivation-cost × attacker-gain dichotomy with a working two-mode artifact that
  *demonstrates its own failure case* (BOUNDS).
- **Genuinely weak / assumption-bound:** no caller authentication (§11), rate cheats
  unbounded, BOUNDS semantic cheats, single-SM throughput, no persistence, TDR
  nuisance. None are fatal **if reported as findings**; all are fatal if hidden
  behind an over-broad claim.
- **What would make the evaluation convincing:** (a) the §14.1 efficacy table with a
  real injected DLL, not just the in-process `cheat_*` command; (b) the §14.2 map
  across operation types; (c) the v1↔v2 ablation on identical attacks; (d) honest
  latency/frame-time showing it is viable only for event-rate state; (e) the §8
  staging re-measurement.

Write the abstract around the invariant (G1 + G5) and the dichotomy. Let BOUNDS mode
be the honesty that makes the REDERIVE result credible.
