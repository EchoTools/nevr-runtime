#include <windows.h>
#include <psapi.h>
#include <cstdint>
#include <cstdio>
#include <string>
#include "pe_convert.h"

// Gamepatches is linked statically into this executable.
#include "common/echovr_functions.h"
#include "initialize.h"

// Read the ImageBase from a PE file's optional header.
// Returns 0 on failure (not a valid PE, can't read, etc).
static uint64_t GetPEImageBase(const char* path) {
    HANDLE hFile = CreateFileA(path, GENERIC_READ, FILE_SHARE_READ,
                               nullptr, OPEN_EXISTING, FILE_ATTRIBUTE_NORMAL, nullptr);
    if (hFile == INVALID_HANDLE_VALUE) return 0;

    // Read DOS header to find NT headers offset
    IMAGE_DOS_HEADER dos = {};
    DWORD bytesRead;
    if (!ReadFile(hFile, &dos, sizeof(dos), &bytesRead, nullptr) ||
        bytesRead < sizeof(dos) || dos.e_magic != 0x5A4D) {
        CloseHandle(hFile);
        return 0;
    }

    // Seek to NT headers
    if (SetFilePointer(hFile, dos.e_lfanew, nullptr, FILE_BEGIN) == INVALID_SET_FILE_POINTER) {
        CloseHandle(hFile);
        return 0;
    }

    IMAGE_NT_HEADERS64 nt = {};
    if (!ReadFile(hFile, &nt, sizeof(nt), &bytesRead, nullptr) ||
        bytesRead < sizeof(nt) || nt.Signature != 0x4550 ||
        nt.OptionalHeader.Magic != IMAGE_NT_OPTIONAL_HDR64_MAGIC) {
        CloseHandle(hFile);
        return 0;
    }

    CloseHandle(hFile);
    return nt.OptionalHeader.ImageBase;
}

// Copy this executable to echovr.exe if we're not already running as echovr.exe.
static void CopySelfToEchovr(const char* selfPath, const char* echovrPath) {
    char selfFull[MAX_PATH], echoFull[MAX_PATH];
    GetFullPathNameA(selfPath, MAX_PATH, selfFull, nullptr);
    GetFullPathNameA(echovrPath, MAX_PATH, echoFull, nullptr);

    if (_stricmp(selfFull, echoFull) == 0)
        return; // Already running as echovr.exe

    if (CopyFileA(selfFull, echoFull, FALSE)) {
        fprintf(stderr, "[NEVR.RUNTIME] Installed as echovr.exe\n");
    } else {
        fprintf(stderr, "[NEVR.RUNTIME] Warning: could not copy to echovr.exe (error %lu)\n",
            GetLastError());
    }
}

static bool FileExists(const char* path) {
    return GetFileAttributesA(path) != INVALID_FILE_ATTRIBUTES;
}

int WINAPI WinMain(HINSTANCE hInst, HINSTANCE, LPSTR cmdLine, int nShow) {
    // Resolve our own path and directory
    char selfPath[MAX_PATH];
    GetModuleFileNameA(NULL, selfPath, MAX_PATH);

    char exeDir[MAX_PATH];
    memcpy(exeDir, selfPath, MAX_PATH);
    char* lastSlash = strrchr(exeDir, '\\');
    if (lastSlash) *(lastSlash + 1) = '\0';

    std::string echovrExe  = std::string(exeDir) + "echovr.exe";
    std::string originalExe = std::string(exeDir) + "echovr_original.exe";
    std::string gameDll     = std::string(exeDir) + "echovr_game.dll";
    std::string entryFile   = std::string(exeDir) + "echovr_game.entry";

    uint32_t entryRva = 0;

    if (FileExists(gameDll.c_str())) {
        // State 1: Already set up — load saved entry RVA
        FILE* f = fopen(entryFile.c_str(), "rb");
        if (!f || fread(&entryRva, sizeof(entryRva), 1, f) != 1) {
            MessageBoxA(NULL, "echovr_game.entry missing or corrupt.\n"
                "Delete echovr_game.dll to re-convert.",
                "NEVR Runtime", MB_ICONERROR);
            if (f) fclose(f);
            return 1;
        }
        fclose(f);

    } else if (FileExists(originalExe.c_str())) {
        // State 2: Original already renamed, just need conversion
        fprintf(stderr, "[NEVR.RUNTIME] Converting echovr_original.exe to DLL...\n");
        if (!ConvertExeToDll(originalExe.c_str(), gameDll.c_str(), &entryRva)) {
            MessageBoxA(NULL, "Failed to convert game executable to DLL.\n"
                "Check console output for details.",
                "NEVR Runtime", MB_ICONERROR);
            return 1;
        }

        FILE* f = fopen(entryFile.c_str(), "wb");
        if (f) { fwrite(&entryRva, sizeof(entryRva), 1, f); fclose(f); }

        fprintf(stderr, "[NEVR.RUNTIME] Conversion complete. Entry RVA: 0x%08X\n", entryRva);
        CopySelfToEchovr(selfPath, echovrExe.c_str());

    } else if (FileExists(echovrExe.c_str())) {
        // State 3: First-time setup — check if echovr.exe is the original game
        uint64_t imageBase = GetPEImageBase(echovrExe.c_str());
        if (imageBase != 0x140000000ULL) {
            MessageBoxA(NULL, "echovr.exe does not appear to be the original game.\n"
                "Place nevr-runtime.exe in the EchoVR game directory.",
                "NEVR Runtime", MB_ICONERROR);
            return 1;
        }

        fprintf(stderr, "[NEVR.RUNTIME] First-time setup\n");

        // Rename original game
        fprintf(stderr, "[NEVR.RUNTIME] Renaming echovr.exe -> echovr_original.exe\n");
        if (!MoveFileA(echovrExe.c_str(), originalExe.c_str())) {
            fprintf(stderr, "[NEVR.RUNTIME] Failed to rename echovr.exe (error %lu)\n", GetLastError());
            MessageBoxA(NULL, "Failed to rename echovr.exe.\n"
                "Make sure the game is not running and try again.",
                "NEVR Runtime", MB_ICONERROR);
            return 1;
        }

        // Convert to DLL
        fprintf(stderr, "[NEVR.RUNTIME] Converting echovr_original.exe to DLL...\n");
        if (!ConvertExeToDll(originalExe.c_str(), gameDll.c_str(), &entryRva)) {
            // Undo the rename so we don't leave a broken state
            MoveFileA(originalExe.c_str(), echovrExe.c_str());
            MessageBoxA(NULL, "Failed to convert game executable to DLL.\n"
                "Check console output for details.",
                "NEVR Runtime", MB_ICONERROR);
            return 1;
        }

        FILE* f = fopen(entryFile.c_str(), "wb");
        if (f) { fwrite(&entryRva, sizeof(entryRva), 1, f); fclose(f); }

        fprintf(stderr, "[NEVR.RUNTIME] Conversion complete. Entry RVA: 0x%08X\n", entryRva);

        // Copy self to echovr.exe
        CopySelfToEchovr(selfPath, echovrExe.c_str());
        fprintf(stderr, "[NEVR.RUNTIME] Setup complete. You can now run echovr.exe directly.\n");

    } else {
        // State 4: No game binary found
        MessageBoxA(NULL, "No game executable found.\n\n"
            "Place nevr-runtime.exe in the EchoVR game directory\n"
            "(next to echovr.exe).",
            "NEVR Runtime", MB_ICONERROR);
        return 1;
    }

    // Reserve the game's preferred VA range so LoadLibrary places it there.
    // The game binary was compiled with ImageBase=0x140000000 and DYNAMIC_BASE=0.
    // Without this reservation trick, Wine may relocate the DLL to a random address,
    // which breaks the game's own internal code that uses hardcoded addresses.
    {
        uint64_t gameImageBase = GetPEImageBase(gameDll.c_str());
        if (gameImageBase == 0) gameImageBase = 0x140000000ULL;

        // VirtualAlloc at the preferred base to check if it's free
        void* probe = VirtualAlloc(reinterpret_cast<void*>(gameImageBase),
                                   0x10000, MEM_RESERVE, PAGE_NOACCESS);
        if (probe) {
            // Range is free — release it so LoadLibrary can use it
            VirtualFree(probe, 0, MEM_RELEASE);
            fprintf(stderr, "[NEVR.RUNTIME] VA 0x%llx is available for game DLL\n",
                static_cast<unsigned long long>(gameImageBase));
        } else {
            fprintf(stderr, "[NEVR.RUNTIME] WARNING: VA 0x%llx is occupied, game may be relocated\n",
                static_cast<unsigned long long>(gameImageBase));
        }
    }

    // Load the game DLL
    HMODULE hGame = LoadLibraryA(gameDll.c_str());
    if (!hGame) {
        fprintf(stderr, "[NEVR.RUNTIME] Failed to load echovr_game.dll (error %lu)\n", GetLastError());
        MessageBoxA(NULL, "Failed to load echovr_game.dll.", "NEVR Runtime", MB_ICONERROR);
        return 1;
    }

    uint64_t actualBase = reinterpret_cast<uint64_t>(hGame);
    fprintf(stderr, "[NEVR.RUNTIME] Game DLL loaded at 0x%llx\n",
        static_cast<unsigned long long>(actualBase));

    if (actualBase != 0x140000000ULL) {
        fprintf(stderr, "[NEVR.RUNTIME] FATAL: Game DLL relocated (expected 0x140000000, got 0x%llx)\n"
            "[NEVR.RUNTIME] The game has hardcoded addresses and cannot run at a different base.\n"
            "[NEVR.RUNTIME] This is a known Wine limitation. Use the dbgcore.dll deployment method instead.\n",
            static_cast<unsigned long long>(actualBase));
        MessageBoxA(NULL,
            "Game DLL loaded at wrong address (Wine relocated it).\n\n"
            "The game cannot run as a DLL under Wine due to address\n"
            "space layout. Use the legacy dbgcore.dll method instead:\n\n"
            "Copy gamepatches.dll as dbgcore.dll next to echovr.exe.",
            "NEVR Runtime", MB_ICONERROR);
        FreeLibrary(hGame);
        return 1;
    }

    // Initialize gamepatches (linked statically into this exe)
    EchoVR::g_GameBaseAddress = (CHAR*)hGame;
    Initialize();
    fprintf(stderr, "[NEVR.RUNTIME] Patches initialized\n");

    // Call game's real entry point.
    // AddressOfEntryPoint is the CRT startup stub (WinMainCRTStartup), not WinMain.
    // The CRT stub calls GetCommandLineW() internally and initializes the CRT before
    // calling WinMain. Our injected stub DllMain prevented the original entry from
    // running during LoadLibrary, so this is the first and only CRT initialization.
    MODULEINFO modInfo = {};
    if (GetModuleInformation(GetCurrentProcess(), hGame, &modInfo, sizeof(modInfo))) {
        if (entryRva >= modInfo.SizeOfImage) {
            fprintf(stderr, "[NEVR.RUNTIME] Entry RVA 0x%08X exceeds module size 0x%08X\n",
                entryRva, (uint32_t)modInfo.SizeOfImage);
            MessageBoxA(NULL, "Invalid entry point. Delete echovr_game.dll to re-convert.",
                "NEVR Runtime", MB_ICONERROR);
            return 1;
        }
    }

    typedef int (*CrtEntry_fn)();
    auto gameEntry = (CrtEntry_fn)((char*)hGame + entryRva);

    fprintf(stderr, "[NEVR.RUNTIME] Calling game entry point at 0x%p\n", (void*)gameEntry);
    return gameEntry();
}
