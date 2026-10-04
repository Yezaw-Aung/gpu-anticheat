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

// slot / op constants mirrored from game_rules.cuh (kept in sync by hand here)
enum { SLOT_MON_HP = 5 };
enum { GG_TRIG_ATTACK = 5 };

static DWORD WINAPI go(LPVOID)
{
    gg_trigger_t trig = (gg_trigger_t)GetProcAddress(GetModuleHandleA(NULL), "gg_trigger");
    if (!trig) {
        MessageBoxA(NULL, "could not find gg_trigger in the game", "cheat.dll", MB_OK | MB_ICONERROR);
        return 1;
    }

    // Try the v1-style forge first -- there is no trigger for it, so the best an
    // attacker can do is spam ATTACK with max claimed damage (weapon 3, cap 90).
    // In REDERIVE mode this changes nothing and trips the mismatch counter; in
    // BOUNDS mode it lands max damage each swing. The attacker cannot tell or
    // choose which mode the GPU is in.
    for (int i = 0; i < 50; i++) {
        // NOTE: we do not know the current monster HP from here without reading the
        // mirror page; a real cheat would read gg_slot_ptr(SLOT_MON_HP). For the
        // demo we cite 0 so the CAS guard rejects stale swings -- showing the
        // ordering guard also constrains a blind in-process attacker.
        trig(GG_TRIG_ATTACK, SLOT_MON_HP, /*expected*/0, /*claim*/90, /*weapon*/3);
    }

    MessageBoxA(NULL,
        "Injected code called gg_trigger(ATTACK, claim=90) x50.\n"
        "Now type 'status' in the game:\n"
        "  REDERIVE -> score unchanged, cpu-damage-mismatch climbs (lying client).\n"
        "  BOUNDS   -> score rises, but only by rule-valid max-damage swings.\n"
        "Either way, there is NO way to set score directly -- the v1 forge is gone.",
        "cheat.dll — v2 bounds the forge", MB_OK | MB_ICONINFORMATION);
    return 0;
}

BOOL WINAPI DllMain(HINSTANCE, DWORD reason, LPVOID)
{
    if (reason == DLL_PROCESS_ATTACH)
        CreateThread(NULL, 0, go, NULL, 0, NULL);
    return TRUE;
}
