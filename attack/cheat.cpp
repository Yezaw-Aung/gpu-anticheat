// cheat.cpp — the injected code (T1). Built as cheat.dll and injected into the
// running game.exe. Once inside the process, it resolves the game's own gg_submit
// and calls it to forge a protected value.
//
// This is the confused-deputy / direct-forge demonstration for v1: the injected
// code drives the game's EXISTING write channel (same CUDA context, same ring,
// same doorbell), so the GPU engine cannot tell it from the game. The value is
// changed with NO tamper alert -- exactly v1's structural weakness.
//
// Authorized use only: this targets your own game process on your own machine to
// demonstrate the vulnerability your paper studies. It is the realistic version
// of the game's built-in `forge` command.
//
// Build (x64 Native Tools Command Prompt for VS 2022):
//     cl /LD cheat.cpp user32.lib
//
// gg_submit signature (from gpuguard.h):  uint64_t gg_submit(int slot, int op, int32_t arg)
// v1 op enum:  GG_OP_ADD = 0, GG_OP_SET = 1
// slots in the penny game:  0 = pennies, 1 = dollars  (construction order)

#include <windows.h>
#include <cstdint>

typedef uint64_t (*gg_submit_t)(int slot, int op, int32_t arg);

static DWORD WINAPI go(LPVOID)
{
    // The game statically links gg_submit into game.exe and (for this demo)
    // exports it, so we resolve it from the main module. A real cheat would find
    // this address by signature scanning; the capability is the same.
    gg_submit_t submit = (gg_submit_t)GetProcAddress(GetModuleHandleA(NULL), "gg_submit");
    if (!submit) {
        MessageBoxA(NULL, "could not find gg_submit in the game", "cheat.dll", MB_OK | MB_ICONERROR);
        return 1;
    }

    // Forge slot 0 (pennies) to 999999 via the game's own write channel.
    //   GG_OP_SET = 1
    submit(0, 1, 999999);

    MessageBoxA(NULL,
        "Injected code called gg_submit(SET pennies = 999999).\n"
        "Now type 'score' and 'status' in the game:\n"
        "  pennies = 999999, alert = 0  ->  forged, undetected (the v1 hole).",
        "cheat.dll — forged via injection", MB_OK | MB_ICONINFORMATION);
    return 0;
}

BOOL WINAPI DllMain(HINSTANCE, DWORD reason, LPVOID)
{
    if (reason == DLL_PROCESS_ATTACH)
        CreateThread(NULL, 0, go, NULL, 0, NULL);   // don't block the loader
    return TRUE;
}
