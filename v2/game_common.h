#pragma once
// game_common.h — shared host-side helpers for both frontends (terminal + gfx).
//
// The protected slots MUST be allocated in the exact SLOT_* order from
// game_rules.cuh so a slot index equals its enum/macro. Both game.cpp and
// game_gfx.cpp call gg_setup_slots() during trusted startup (before gg_start)
// to guarantee that, instead of each maintaining its own (drift-prone) order.

#include <cstdint>
#include <random>
#include "gpuguard.h"
#include "game_rules.cuh"

// Monster spawn positions. Monster 0 starts adjacent to the player (16,16) so the
// terminal demo's melee ATTACK lands without moving; the rest ring the arena.
static const int GG_SPAWNX[GG_NMON] = { 17, 4, 28, 4,  28 };
static const int GG_SPAWNY[GG_NMON] = { 16, 4, 4,  28, 28 };

// Allocate every protected slot in canonical order with its initial value.
static inline void gg_setup_slots()
{
    gg_alloc_slot(100);      // SLOT_HP
    gg_alloc_slot(12);       // SLOT_AMMO
    gg_alloc_slot(16);       // SLOT_POSX
    gg_alloc_slot(16);       // SLOT_POSY
    gg_alloc_slot(0);        // SLOT_WEAPON_CD
    gg_alloc_slot(0);        // SLOT_BUFF
    gg_alloc_slot(0x1234);   // SLOT_RNG
    gg_alloc_slot(0);        // SLOT_SCORE
    for (int i = 0; i < GG_NMON; i++) {
        gg_alloc_slot(GG_MON_HP_INIT);           // MON_HP(i)
        gg_alloc_slot(GG_MON_ARMOR_INIT);        // MON_ARMOR(i)
        gg_alloc_slot((uint32_t)GG_SPAWNX[i]);   // MON_POSX(i)
        gg_alloc_slot((uint32_t)GG_SPAWNY[i]);   // MON_POSY(i)
    }
}

// Read a protected slot's current (GPU-healed) value.
static inline int gg_rd(int slot)
{
    volatile uint32_t* p = gg_slot_ptr(slot);
    return p ? (int)*p : 0;
}

// The CPU-heavy "computation": an honest client rolls within the weapon's cap;
// a T1 cheat always claims the maximum. REDERIVE ignores this; BOUNDS trusts it.
static inline int32_t gg_cpu_attack_calc(std::mt19937& rng, int weapon, bool cheat)
{
    static const int32_t cap[GG_NWEAPONS] = { 25, 40, 60, 90 };
    if (weapon < 0 || weapon >= GG_NWEAPONS) weapon = 0;
    if (cheat) return cap[weapon];
    std::uniform_int_distribution<int> d(0, cap[weapon]);
    return d(rng);
}
