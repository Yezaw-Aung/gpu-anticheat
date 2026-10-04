// injector.cpp — a minimal DLL injector (the standard CreateRemoteThread +
// LoadLibrary technique). Finds the game process by name and loads cheat.dll into
// it, which is what gets the injected code running in-process (T1).
//
// Authorized use only: injects into your own game process on your own machine to
// demonstrate the vulnerability your paper studies.
//
// Build (x64 Native Tools Command Prompt for VS 2022):
//     cl injector.cpp
//
// Run (after starting game.exe):
//     injector.exe game.exe cheat.dll

#include <windows.h>
#include <tlhelp32.h>
#include <cstdio>
#include <cstring>

static DWORD find_pid(const char *name)
{
    DWORD pid = 0;
    HANDLE snap = CreateToolhelp32Snapshot(TH32CS_SNAPPROCESS, 0);
    if (snap == INVALID_HANDLE_VALUE) return 0;
    PROCESSENTRY32 pe; pe.dwSize = sizeof(pe);
    if (Process32First(snap, &pe)) {
        do {
            if (_stricmp(pe.szExeFile, name) == 0) { pid = pe.th32ProcessID; break; }
        } while (Process32Next(snap, &pe));
    }
    CloseHandle(snap);
    return pid;
}

int main(int argc, char **argv)
{
    if (argc < 3) {
        printf("usage: injector.exe <game exe name> <dll path>\n"
               "  e.g. injector.exe game.exe cheat.dll\n");
        return 1;
    }
    const char *procName = argv[1];

    // Absolute path to the DLL (LoadLibrary in the target needs a resolvable path).
    char dllPath[MAX_PATH];
    if (!GetFullPathNameA(argv[2], MAX_PATH, dllPath, NULL)) {
        printf("bad dll path\n"); return 1;
    }
    if (GetFileAttributesA(dllPath) == INVALID_FILE_ATTRIBUTES) {
        printf("dll not found: %s\n", dllPath); return 1;
    }

    DWORD pid = find_pid(procName);
    if (!pid) { printf("process '%s' not found -- is the game running?\n", procName); return 1; }

    HANDLE proc = OpenProcess(PROCESS_ALL_ACCESS, FALSE, pid);
    if (!proc) { printf("OpenProcess failed (%lu)\n", GetLastError()); return 1; }

    // Write the DLL path into the target, then run LoadLibraryA(path) as a remote
    // thread. kernel32 is at the same address in every process, so LoadLibraryA's
    // address here is valid in the target.
    size_t n = strlen(dllPath) + 1;
    void *mem = VirtualAllocEx(proc, NULL, n, MEM_COMMIT | MEM_RESERVE, PAGE_READWRITE);
    if (!mem) { printf("VirtualAllocEx failed (%lu)\n", GetLastError()); return 1; }
    if (!WriteProcessMemory(proc, mem, dllPath, n, NULL)) {
        printf("WriteProcessMemory failed (%lu)\n", GetLastError()); return 1;
    }
    LPTHREAD_START_ROUTINE loadlib =
        (LPTHREAD_START_ROUTINE)GetProcAddress(GetModuleHandleA("kernel32.dll"), "LoadLibraryA");
    HANDLE th = CreateRemoteThread(proc, NULL, 0, loadlib, mem, 0, NULL);
    if (!th) { printf("CreateRemoteThread failed (%lu)\n", GetLastError()); return 1; }

    WaitForSingleObject(th, INFINITE);
    printf("injected %s into '%s' (pid %lu)\n", dllPath, procName, pid);
    VirtualFreeEx(proc, mem, 0, MEM_RELEASE);
    CloseHandle(th);
    CloseHandle(proc);
    return 0;
}
