// cheat.cpp — the injected code (T1) for v2. Built as cheat.dll and injected into
// the running game.exe. Once inside the process it resolves the game's own
// gg_trigger and drives it -- the realistic version of the game's cheat_* commands.
//
// THE V2 CONTRAST (this is the paper's point). In v1, injected code called
// gg_submit(slot, SET, 999999) and FORGED any value, no alert. In v2 there is no
// such call. The strongest thing this DLL can do is submit RULE-VALID triggers:
//
//   1. It CANNOT set score directly -- no trigger writes score. The v1 forge is
//      simply not expressible.
//   2. It CAN submit ATTACK with max claimed damage. Whether that helps depends on
//      the GPU's combat mode, which the attacker does NOT control:
//        * GG_MODE_BOUNDS   -> max damage accepted every swing. Cheat works, but
//          only within the rules (bounded DPS), and leaves a mismatch=0 signature.
//        * GG_MODE_REDERIVE -> the claim is ignored; damage is the GPU's own roll.
//          The cheat gains nothing, and every lie increments the GPU's mismatch
//          counter -- a detectable signature of a lying client.
//
// Authorized use only: targets your own game.exe on your own machine to
// demonstrate the vulnerability (and its v2 bound) your paper studies.
//
// Build (x64 Native Tools Command Prompt for VS 2022):   cl /LD cheat.cpp user32.lib
//
// gg_trigger signature (gpuguard.h):
//   uint64_t gg_trigger(int op, int32_t a0, int32_t a1, int32_t a2, int32_t a3)
// ATTACK: op=5, a0=SLOT_MON_HP(=5), a1=expected_monster_hp, a2=claimed_damage, a3=weapon

#include <windows.h>
#include <cstdint>

typedef uint64_t (*gg_trigger_t)(int, int32_t, int32_t, int32_t, int32_t);

// op / stat constants mirrored from game_rules.cuh (kept in sync by hand here).
// ATTACK args: arg0 = monster INDEX, arg1 = expected monster HP (CAS),
//              arg2 = claimed damage, arg3 = weapon.
enum { GG_TRIG_ATTACK = 5 };
enum { GG_MON_HP_INIT = 60 };   // a freshly spawned monster's HP

static DWORD WINAPI go(LPVOID)
{
    gg_trigger_t trig = (gg_trigger_t)GetProcAddress(GetModuleHandleA(NULL), "gg_trigger");
    if (!trig) {
        MessageBoxA(NULL, "could not find gg_trigger in the game", "cheat.dll", MB_OK | MB_ICONERROR);
        return 1;
    }

    // There is no trigger that writes score, so the best an in-process attacker can
    // do is spam melee ATTACK with max claimed damage (weapon 3, cap 90) on
    // monster 0. v2 bounds this THREE ways the attacker cannot avoid:
    //   1. combat mode: REDERIVE ignores the claim (mismatch climbs); only BOUNDS
    //      trusts it. The attacker cannot choose the mode.
    //   2. RANGE: the GPU rejects the swing unless the player is within melee range
    //      of monster 0 -- a spatial rule checked from authoritative positions.
    //   3. CAS: each swing must cite the current monster HP; once HP moves, stale
    //      swings are rejected. We cite the fresh-spawn HP, so at most the first
    //      lands (if in range); the rest show the guard rejecting a blind attacker.
    for (int i = 0; i < 50; i++)
        trig(GG_TRIG_ATTACK, /*monster*/0, /*expected*/GG_MON_HP_INIT, /*claim*/90, /*weapon*/3);

    MessageBoxA(NULL,
        "Injected code called gg_trigger(ATTACK monster 0, claim=90) x50.\n"
        "Now type 'status' / 'look' in the game:\n"
        "  REDERIVE -> claim ignored, cpu-damage-mismatch climbs (lying client).\n"
        "  BOUNDS   -> lands only while in range AND HP matches (rule-valid swings).\n"
        "There is NO way to set score directly, and the GPU range-gates the hit.",
        "cheat.dll - v2 bounds the forge", MB_OK | MB_ICONINFORMATION);
    return 0;
}

BOOL WINAPI DllMain(HINSTANCE, DWORD reason, LPVOID)
{
    if (reason == DLL_PROCESS_ATTACH)
        CreateThread(NULL, 0, go, NULL, 0, NULL);
    return TRUE;
}
