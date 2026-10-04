// game_rules.cuh — the GPU-resident rule core.  [VERSION 2: rule-reward engine]
//
// Compiled UNCHANGED by nvcc (the device engine in gpuguard.cu) and by an ordinary
// C++ compiler (the CPU stub). gg_apply_rule() is PURE -- it touches only the
// shadow array, the rule table, and the request -- which is why it compiles for
// both and why the enforced rule and the simulated rule are literally one source.
//
// *** THE V2 MODEL ***
// No raw write path. The host cannot name a protected value; it submits a TRIGGER
// and the GPU decides the result from its OWN authoritative prior state + this
// rule table. See DESIGN_v2.md for the threat model and the exact invariant.
//
// This version models a small top-down arena: a player and GG_NMON monsters. The
// GPU owns every security-sensitive value, now INCLUDING monster positions, so it
// can enforce spatial rules (melee range) that the CPU cannot forge:
//   * melee ATTACK is REJECTED unless the player is within melee range of the
//     target monster, checked against GPU-authoritative positions;
//   * monsters chase the player and deal contact damage via the GPU's own rule;
//   * bullets are proposed by the CPU (BULLET_HIT) and verified by the GPU.
// Honest limits (DESIGN_v2.md): monster MOVEMENT TEMPO is CPU-driven (the host
// decides when to send MONSTER_STEP) -> a rate cheat the GPU cannot bound; and a
// bullet's TRAJECTORY is CPU-computed -> the GPU verifies liveness, range and
// damage, but trusts that a bullet geometrically reached the monster.

#ifndef GAME_RULES_CUH
#define GAME_RULES_CUH

#include <stdint.h>

#if defined(__CUDACC__)
#define GG_FN __host__ __device__ __forceinline__
#else
#define GG_FN inline
#endif

// ---------------------------------------------------------------- state layout
// Fixed player/global slots, then a block of 4 slots per monster.
enum {
    SLOT_HP = 0,     // player hit points
    SLOT_AMMO,       // rounds in the magazine
    SLOT_POSX,       // player x (grid)
    SLOT_POSY,       // player y (grid)
    SLOT_WEAPON_CD,  // frames until the weapon can fire again (0 = ready)
    SLOT_BUFF,       // 1 if a damage buff is active (authoritative)
    SLOT_RNG,        // GPU-owned RNG state (CPU cannot predict/set)
    SLOT_SCORE,      // GPU-OWNED reward; rises ONLY on a GPU-adjudicated kill
    SLOT_MON_BASE    // first monster slot
};

#define GG_NMON 5                       // number of monsters
#define GG_NWEAPONS 4

// per-monster slots (i = 0 .. GG_NMON-1)
#define MON_HP(i)    (SLOT_MON_BASE + (i)*4 + 0)
#define MON_ARMOR(i) (SLOT_MON_BASE + (i)*4 + 1)
#define MON_POSX(i)  (SLOT_MON_BASE + (i)*4 + 2)
#define MON_POSY(i)  (SLOT_MON_BASE + (i)*4 + 3)

#define SLOT_COUNT   (SLOT_MON_BASE + GG_NMON * 4)   // = 8 + 5*4 = 28

// monster spawn/respawn stats (also used by the host to seed initial slots)
#define GG_MON_HP_INIT    60
#define GG_MON_ARMOR_INIT 3

// --------------------------------------------------------------- the triggers
enum {
    GG_TRIG_MOVE = 0,     // arg0 = dir (0=left,1=right,2=up,3=down)
    GG_TRIG_SHOOT,        // consume ammo if ready (bullets are CPU-simulated)
    GG_TRIG_RELOAD,
    GG_TRIG_TICK,         // per-frame housekeeping (cooldown--, RNG step)
    GG_TRIG_USE_ITEM,     // spend score to heal, capped
    GG_TRIG_ATTACK,       // MELEE, range-gated. arg0=monster index,
                          //   arg1=expected monster HP (CAS), arg2=claimed damage,
                          //   arg3=weapon
    GG_TRIG_BULLET_HIT,   // RANGED. same args as ATTACK; bullet-range gated
    GG_TRIG_MONSTER_STEP, // move every living monster toward the player + contact dmg
    GG_TRIG_COUNT
};

// combat_mode -- the core experiment lever (DESIGN_v2.md section 6).
enum {
    GG_MODE_REDERIVE = 0, // GPU recomputes damage from its own roll; claim ignored
    GG_MODE_BOUNDS   = 1  // GPU trusts claimed damage within max_dmg[weapon]
};

// --------------------------------------------------------------- the rule table
struct Rules {
    uint32_t combat_mode;

    int32_t  move_step;
    int32_t  bound_min, bound_max;

    int32_t  ammo_per_shot;
    int32_t  weapon_cooldown;
    int32_t  max_ammo;

    int32_t  max_hp;
    int32_t  item_cost;
    int32_t  item_heal;

    int32_t  dmg_base[GG_NWEAPONS];
    int32_t  dmg_var [GG_NWEAPONS];
    int32_t  max_dmg [GG_NWEAPONS];
    int32_t  buff_bonus;

    int32_t  kill_reward;
    int32_t  mon_hp_max;
    int32_t  mon_armor;

    int32_t  melee_range2;    // squared melee range (grid cells^2)
    int32_t  bullet_range2;   // squared bullet sanity range
    int32_t  contact_range2;  // squared range at which a monster damages the player
    int32_t  contact_dmg;     // HP lost per adjacent monster per MONSTER_STEP
};

struct Trig {
    uint32_t seq;
    uint32_t op;
    int32_t  arg0, arg1, arg2, arg3;
};

GG_FN uint32_t gg_rng_next(uint32_t s) { return s * 1664525u + 1013904223u; }

// Compute the weapon damage for a hit. In REDERIVE mode the GPU rolls its own
// damage and IGNORES the claim (counting disagreements as mismatches); in BOUNDS
// mode it trusts the claim within [0, max_dmg[w]]. Returns -1 to signal an illegal
// claim (bounds mode) so the caller rejects.
GG_FN int32_t gg_weapon_damage(uint32_t* sh, const Rules* R, int w,
                               int32_t claim, uint32_t* out_mismatch)
{
    int32_t dmg;
    if (R->combat_mode == GG_MODE_REDERIVE) {
        sh[SLOT_RNG] = gg_rng_next(sh[SLOT_RNG]);
        int32_t roll = (int32_t)((sh[SLOT_RNG] >> 16) % 100u);   // 0..99
        dmg = R->dmg_base[w] + (R->dmg_var[w] * roll) / 100;
        if (sh[SLOT_BUFF]) dmg += R->buff_bonus;
        if (out_mismatch && claim != dmg) (*out_mismatch)++;
    } else {
        dmg = claim;
        if (dmg < 0 || dmg > R->max_dmg[w]) return -1;           // illegal claim
    }
    return dmg;
}

// Apply `dmg` (pre-armor) to monster i, then handle death: grant the GPU-owned
// kill reward and respawn the monster at a pseudo-random position. PURE.
GG_FN void gg_damage_monster(uint32_t* sh, const Rules* R, int i, int32_t dmg)
{
    dmg -= (int32_t)sh[MON_ARMOR(i)];
    if (dmg < 0) dmg = 0;
    int32_t hp = (int32_t)sh[MON_HP(i)] - dmg;
    if (hp < 0) hp = 0;
    sh[MON_HP(i)] = (uint32_t)hp;

    if (hp == 0) {
        sh[SLOT_SCORE] += (uint32_t)R->kill_reward;              // GPU-OWNED reward
        // respawn at a GPU-chosen position (CPU cannot place it)
        sh[SLOT_RNG] = gg_rng_next(sh[SLOT_RNG]);
        uint32_t r = sh[SLOT_RNG];
        int span = R->bound_max - R->bound_min + 1;
        int nx = R->bound_min + (int)((r >> 8)  % (uint32_t)span);
        int ny = R->bound_min + (int)((r >> 17) % (uint32_t)span);
        sh[MON_POSX(i)]  = (uint32_t)nx;
        sh[MON_POSY(i)]  = (uint32_t)ny;
        sh[MON_HP(i)]    = (uint32_t)R->mon_hp_max;
        sh[MON_ARMOR(i)] = (uint32_t)R->mon_armor;
    }
}

// Shared body of melee ATTACK and ranged BULLET_HIT: validate target, CAS, and a
// squared-distance range gate against AUTHORITATIVE positions, then damage.
GG_FN int gg_hit_monster(uint32_t* sh, const Rules* R, const Trig* t,
                         int32_t range2, uint32_t* out_mismatch)
{
    int i = (int)t->arg0;
    if (i < 0 || i >= GG_NMON) return 0;
    if (sh[MON_HP(i)] == 0) return 0;                            // already dead
    if (sh[MON_HP(i)] != (uint32_t)t->arg1) return 0;           // stale CAS

    int w = (int)t->arg3;
    if (w < 0 || w >= GG_NWEAPONS) return 0;

    // RANGE GATE (GPU-enforced, from authoritative positions). A T1 attacker
    // cannot hit a monster it is not actually near -- this is the spatial rule
    // the CPU is not trusted to check.
    int dx = (int)sh[SLOT_POSX] - (int)sh[MON_POSX(i)];
    int dy = (int)sh[SLOT_POSY] - (int)sh[MON_POSY(i)];
    if (dx*dx + dy*dy > range2) return 0;                       // out of range

    int32_t dmg = gg_weapon_damage(sh, R, w, t->arg2, out_mismatch);
    if (dmg < 0) return 0;                                      // illegal claim
    gg_damage_monster(sh, R, i, dmg);
    return 1;
}

// ---------------------------------------------------------------- THE RULES
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
        else return 0;
        if (nx < R->bound_min || nx > R->bound_max ||
            ny < R->bound_min || ny > R->bound_max) return 0;
        sh[SLOT_POSX] = (uint32_t)nx;
        sh[SLOT_POSY] = (uint32_t)ny;
        return 1;
    }

    case GG_TRIG_SHOOT: {
        if ((int32_t)sh[SLOT_AMMO] < R->ammo_per_shot) return 0;
        if (sh[SLOT_WEAPON_CD] != 0)                   return 0;
        sh[SLOT_AMMO]      -= (uint32_t)R->ammo_per_shot;
        sh[SLOT_WEAPON_CD]  = (uint32_t)R->weapon_cooldown;
        return 1;
    }

    case GG_TRIG_RELOAD:
        sh[SLOT_AMMO]      = (uint32_t)R->max_ammo;
        sh[SLOT_WEAPON_CD] = 0;
        return 1;

    case GG_TRIG_TICK:
        if (sh[SLOT_WEAPON_CD] > 0) sh[SLOT_WEAPON_CD] -= 1;
        sh[SLOT_RNG] = gg_rng_next(sh[SLOT_RNG]);
        return 1;

    case GG_TRIG_USE_ITEM: {
        if ((int32_t)sh[SLOT_SCORE] < R->item_cost) return 0;
        if ((int32_t)sh[SLOT_HP]   >= R->max_hp)    return 0;
        sh[SLOT_SCORE] -= (uint32_t)R->item_cost;
        int32_t hp = (int32_t)sh[SLOT_HP] + R->item_heal;
        if (hp > R->max_hp) hp = R->max_hp;
        sh[SLOT_HP] = (uint32_t)hp;
        return 1;
    }

    case GG_TRIG_ATTACK:      // melee: tight range gate
        return gg_hit_monster(sh, R, t, R->melee_range2, out_mismatch);

    case GG_TRIG_BULLET_HIT:  // ranged: generous range gate
        return gg_hit_monster(sh, R, t, R->bullet_range2, out_mismatch);

    case GG_TRIG_MONSTER_STEP: {
        int px = (int)sh[SLOT_POSX], py = (int)sh[SLOT_POSY];
        for (int i = 0; i < GG_NMON; i++) {
            if (sh[MON_HP(i)] == 0) continue;
            int mx = (int)sh[MON_POSX(i)], my = (int)sh[MON_POSY(i)];
            if      (mx < px) mx++;  else if (mx > px) mx--;     // chase
            if      (my < py) my++;  else if (my > py) my--;
            if (mx < R->bound_min) mx = R->bound_min;
            if (mx > R->bound_max) mx = R->bound_max;
            if (my < R->bound_min) my = R->bound_min;
            if (my > R->bound_max) my = R->bound_max;
            sh[MON_POSX(i)] = (uint32_t)mx;
            sh[MON_POSY(i)] = (uint32_t)my;
            int cdx = px - mx, cdy = py - my;
            if (cdx*cdx + cdy*cdy <= R->contact_range2) {        // contact damage
                int32_t hp = (int32_t)sh[SLOT_HP] - R->contact_dmg;
                if (hp < 0) hp = 0;
                sh[SLOT_HP] = (uint32_t)hp;
            }
        }
        return 1;
    }

    default:
        return 0;
    }
}

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
    R->kill_reward    = 50;
    R->mon_hp_max     = GG_MON_HP_INIT;
    R->mon_armor      = GG_MON_ARMOR_INIT;
    R->melee_range2   = 9;     // within 3 cells
    R->bullet_range2  = 900;   // within 30 cells (whole arena; bound, not block)
    R->contact_range2 = 2;     // adjacent (incl. diagonal)
    R->contact_dmg    = 4;
}

#endif // GAME_RULES_CUH
