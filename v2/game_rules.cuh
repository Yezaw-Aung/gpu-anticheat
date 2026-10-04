// game_rules.cuh — the GPU-resident rule core.  [VERSION 2: rule-reward engine]
//
// This is the single source of truth for every protected transition. It is
// compiled UNCHANGED by two toolchains:
//   * nvcc, into the persistent device engine (gpuguard.cu) -> runs on the GPU,
//     where the shadow state lives in on-chip shared memory the host cannot reach;
//   * an ordinary C++ compiler, into the CPU stub (gpuguard_stub.cpp) -> so the
//     game runs on a laptop with no CUDA, with IDENTICAL semantics (but no
//     protection -- the stub is clearly labelled UNPROTECTED).
//
// Compiling the same function in both places is a deliberate anti-drift measure:
// the rule the GPU enforces and the rule the stub simulates are literally the
// same source lines. gg_apply_rule() is PURE: it touches only the shadow array,
// the rule table, and the request. No printf, no atomics, no CUDA intrinsics --
// which is exactly why it compiles for the host too.
//
// *** THE V2 MODEL, IN ONE SENTENCE ***
// There is no raw write path. The host cannot name a protected value. It can only
// submit a TRIGGER (MOVE/SHOOT/ATTACK/...), and the GPU decides the resulting
// state from its OWN authoritative prior state plus this rule table. See
// DESIGN_v2.md for the threat model and the exact invariant this enforces.

#ifndef GAME_RULES_CUH
#define GAME_RULES_CUH

#include <stdint.h>

// __device__ under nvcc, plain inline for the host stub. The one token that lets
// the same body live in both worlds.
#if defined(__CUDACC__)
#define GG_FN __device__ __forceinline__
#else
#define GG_FN inline
#endif

// ---------------------------------------------------------------- state layout
// The authoritative protected state is a flat array of uint32 "slots". Each slot
// has a fixed semantic meaning below. In v2 the host NEVER writes these; it reads
// a mirror page (for display) that the engine heals every iteration. The true
// values live in GPU shared memory (engine) / a host array (stub, unprotected).
enum {
    SLOT_HP = 0,     // player hit points
    SLOT_AMMO,       // rounds in the magazine
    SLOT_POSX,       // player x (grid)
    SLOT_POSY,       // player y (grid)
    SLOT_WEAPON_CD,  // frames until the weapon can fire again (0 = ready)
    SLOT_MON_HP,     // current monster hit points
    SLOT_MON_ARMOR,  // current monster armor (flat damage reduction)
    SLOT_BUFF,       // 1 if a damage buff is active, else 0 (authoritative)
    SLOT_RNG,        // GPU-owned RNG state (counter-based; CPU cannot predict/set)
    SLOT_SCORE,      // GPU-OWNED reward. Rises ONLY when the engine adjudicates a
                     // kill. There is no trigger that writes it directly.
    SLOT_COUNT
};

// --------------------------------------------------------------- the triggers
// The complete vocabulary the host may speak. Note what is ABSENT: no SET, no ADD,
// no "write score". Compare v1, whose vocabulary was {SET, ADD} on any slot.
enum {
    GG_TRIG_MOVE = 0,   // arg0 = dir (0=left,1=right,2=up,3=down)
    GG_TRIG_SHOOT,      // (no args) consume ammo if ready
    GG_TRIG_RELOAD,     // (no args) refill magazine
    GG_TRIG_TICK,       // (no args) per-frame housekeeping (cooldown--, RNG step)
    GG_TRIG_USE_ITEM,   // (no args) spend score to heal, capped
    GG_TRIG_ATTACK,     // the CPU-HEAVY op. arg0=monster_slot(=SLOT_MON_HP),
                        //   arg1=expected_monster_hp (CAS guard),
                        //   arg2=claimed_damage (used only in BOUNDS mode),
                        //   arg3=weapon_id
    GG_TRIG_COUNT
};

// combat_mode values -- the CORE EXPERIMENT lever (see DESIGN_v2.md section 6).
enum {
    GG_MODE_REDERIVE = 0, // GPU recomputes damage from its OWN roll + rule params;
                          //   the CPU's claimed_damage is IGNORED (only logged).
                          //   This is the only mode in which the GPU is truly the
                          //   authority over combat outcomes. T1 cannot inflate
                          //   damage; it can at best pick the best legal weapon.
    GG_MODE_BOUNDS = 1    // GPU trusts claimed_damage if 0 <= dmg <= max_dmg[w].
                          //   This is the detect-vs-prevent TRAP: every transition
                          //   is "legal", yet a T1 attacker submits max damage on
                          //   every swing. The invariant holds; the cheat succeeds.
};

#define GG_NWEAPONS 4

// --------------------------------------------------------------- the rule table
// Tunable rule PARAMETERS. The rule LOGIC is the code below; the knobs are data.
// In the engine this struct lives in device memory, seeded once at launch, so the
// host cannot edit it after startup (part of what makes the rules "GPU-resident").
// DESIGN_v2.md is explicit that logic-in-code vs fully data-driven rules is a
// spectrum; parameters-in-device-memory is sufficient for the invariant.
struct Rules {
    uint32_t combat_mode;        // GG_MODE_REDERIVE or GG_MODE_BOUNDS

    int32_t  move_step;          // grid cells per MOVE
    int32_t  bound_min, bound_max;

    int32_t  ammo_per_shot;      // ammo consumed per SHOOT
    int32_t  weapon_cooldown;    // frames of cooldown after a SHOOT
    int32_t  max_ammo;           // RELOAD target

    int32_t  max_hp;             // HP cap (USE_ITEM)
    int32_t  item_cost;          // score spent per USE_ITEM
    int32_t  item_heal;          // HP restored per USE_ITEM

    int32_t  dmg_base[GG_NWEAPONS]; // re-derive: base damage per weapon
    int32_t  dmg_var [GG_NWEAPONS]; // re-derive: roll-scaled variance per weapon
    int32_t  max_dmg [GG_NWEAPONS]; // bounds: accepted claimed-damage ceiling
    int32_t  buff_bonus;            // re-derive: flat bonus when SLOT_BUFF != 0

    int32_t  kill_reward;        // score granted by the GPU on a kill
    int32_t  mon_hp_max;         // monster respawn HP
    int32_t  mon_armor;          // monster respawn armor
};

// A single request as it sits in the ring. 24 bytes, one or two loads.
struct Trig {
    uint32_t seq;    // producer sequence; checked against the engine's register head
    uint32_t op;     // GG_TRIG_*
    int32_t  arg0, arg1, arg2, arg3;
};

// counter-based RNG step (LCG). GPU-owned: the attacker cannot set SLOT_RNG (no
// write path) nor predict the next roll without the state, which never leaves the
// GPU. This is what makes re-derive mode's roll un-forgeable.
GG_FN uint32_t gg_rng_next(uint32_t s) { return s * 1664525u + 1013904223u; }

// ---------------------------------------------------------------- THE RULES
// Evaluate one trigger against the authoritative shadow. Returns 1 if the
// transition is valid and was applied to `sh`, 0 if rejected (shadow untouched on
// the paths that reject before mutating). PURE: shadow + rules + request only.
//
// `out_mismatch` (may be null) counts, in re-derive mode, how often the CPU's
// claimed_damage disagreed with the GPU's computed damage -- i.e. how often the
// CPU helper lied or erred. Pure telemetry; it changes no decision.
GG_FN int gg_apply_rule(uint32_t* sh, const Rules* R, const Trig* t,
                        uint32_t* out_mismatch)
{
    switch (t->op) {

    case GG_TRIG_MOVE: {
        int dir = (int)t->arg0;
        int32_t nx = (int32_t)sh[SLOT_POSX];
        int32_t ny = (int32_t)sh[SLOT_POSY];
        if      (dir == 0) nx -= R->move_step;
        else if (dir == 1) nx += R->move_step;
        else if (dir == 2) ny -= R->move_step;
        else if (dir == 3) ny += R->move_step;
        else return 0;                                  // unknown direction
        if (nx < R->bound_min || nx > R->bound_max ||
            ny < R->bound_min || ny > R->bound_max)
            return 0;                                   // out of bounds -> rejected
        sh[SLOT_POSX] = (uint32_t)nx;
        sh[SLOT_POSY] = (uint32_t)ny;
        return 1;
    }

    case GG_TRIG_SHOOT: {
        if ((int32_t)sh[SLOT_AMMO] < R->ammo_per_shot) return 0;  // out of ammo
        if (sh[SLOT_WEAPON_CD] != 0)                    return 0;  // still cooling
        sh[SLOT_AMMO]      -= (uint32_t)R->ammo_per_shot;
        sh[SLOT_WEAPON_CD]  = (uint32_t)R->weapon_cooldown;
        return 1;
    }

    case GG_TRIG_RELOAD: {
        sh[SLOT_AMMO]      = (uint32_t)R->max_ammo;
        sh[SLOT_WEAPON_CD] = 0;
        return 1;
    }

    case GG_TRIG_TICK: {
        // Per-frame housekeeping. NOTE (DESIGN_v2.md section on the clock problem):
        // a T1 attacker can spam TICK to clear weapon cooldown faster than real
        // frames, a legal-but-abusive RATE cheat. This is a deliberate, documented
        // limitation -- the GPU has no trusted wall clock to bound it.
        if (sh[SLOT_WEAPON_CD] > 0) sh[SLOT_WEAPON_CD] -= 1;
        sh[SLOT_RNG] = gg_rng_next(sh[SLOT_RNG]);
        return 1;
    }

    case GG_TRIG_USE_ITEM: {
        if ((int32_t)sh[SLOT_SCORE] < R->item_cost) return 0;     // can't afford
        if ((int32_t)sh[SLOT_HP]   >= R->max_hp)    return 0;     // already full
        sh[SLOT_SCORE] -= (uint32_t)R->item_cost;
        int32_t hp = (int32_t)sh[SLOT_HP] + R->item_heal;
        if (hp > R->max_hp) hp = R->max_hp;
        sh[SLOT_HP] = (uint32_t)hp;
        return 1;
    }

    case GG_TRIG_ATTACK: {
        // CAS / stale guard: the request must cite the CURRENT authoritative
        // monster HP. If another request (or a respawn) moved it, this one is
        // stale and is rejected -- never misapplied. This is how ordering and
        // the legitimate->malicious->legitimate transient race are defeated on
        // the heavy path.
        if ((int32_t)sh[SLOT_MON_HP] != t->arg1) return 0;

        int w = (int)t->arg3;
        if (w < 0 || w >= GG_NWEAPONS) return 0;

        int32_t dmg;
        if (R->combat_mode == GG_MODE_REDERIVE) {
            // TRUE AUTHORITY. Recompute damage from a GPU-owned roll and the rule
            // table; the CPU's claimed_damage (arg2) is IGNORED. The buff is read
            // from the authoritative slot, not from the request. A T1 attacker
            // cannot inflate this -- the only freedom left is choosing a weapon.
            sh[SLOT_RNG] = gg_rng_next(sh[SLOT_RNG]);
            int32_t roll = (int32_t)((sh[SLOT_RNG] >> 16) % 100u);   // 0..99
            dmg = R->dmg_base[w] + (R->dmg_var[w] * roll) / 100;
            if (sh[SLOT_BUFF]) dmg += R->buff_bonus;
            if (out_mismatch && t->arg2 != dmg) (*out_mismatch)++;   // CPU lied/erred
        } else {
            // BOUNDS mode: trust the CPU's claim within a ceiling. This is the
            // trap -- a legal transition that a T1 attacker always maxes out.
            dmg = t->arg2;
            if (dmg < 0 || dmg > R->max_dmg[w]) return 0;
        }

        int32_t armor = (int32_t)sh[SLOT_MON_ARMOR];
        dmg -= armor; if (dmg < 0) dmg = 0;

        int32_t hp = (int32_t)sh[SLOT_MON_HP] - dmg;
        if (hp < 0) hp = 0;
        sh[SLOT_MON_HP] = (uint32_t)hp;

        if (hp == 0) {
            // GPU-OWNED REWARD. Score rises HERE AND NOWHERE ELSE. There is no
            // trigger that writes score, so no in-process code can grant itself
            // score without the GPU first adjudicating a kill from authoritative
            // monster HP. This is v2's cleanest, strongest invariant.
            sh[SLOT_SCORE]    += (uint32_t)R->kill_reward;
            sh[SLOT_MON_HP]    = (uint32_t)R->mon_hp_max;     // respawn (authoritative)
            sh[SLOT_MON_ARMOR] = (uint32_t)R->mon_armor;
        }
        return 1;
    }

    default:
        return 0;   // unknown op -> rejected
    }
}

// Sensible defaults for the demo economy. Shared by engine and stub so both start
// identical. combat_mode is overridden at startup (env var / setter).
GG_FN void gg_default_rules(Rules* R)
{
    R->combat_mode    = GG_MODE_REDERIVE;
    R->move_step      = 1;
    R->bound_min      = 0;
    R->bound_max      = 31;
    R->ammo_per_shot  = 1;
    R->weapon_cooldown= 3;
    R->max_ammo       = 12;
    R->max_hp         = 100;
    R->item_cost      = 50;
    R->item_heal      = 25;
    R->dmg_base[0]=10; R->dmg_base[1]=18; R->dmg_base[2]=30; R->dmg_base[3]=45;
    R->dmg_var [0]=10; R->dmg_var [1]=14; R->dmg_var [2]=20; R->dmg_var [3]=30;
    R->max_dmg [0]=25; R->max_dmg [1]=40; R->max_dmg [2]=60; R->max_dmg [3]=90;
    R->buff_bonus     = 15;
    R->kill_reward    = 100;
    R->mon_hp_max     = 120;
    R->mon_armor      = 5;
}

#endif // GAME_RULES_CUH
