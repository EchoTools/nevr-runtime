#include <windows.h>
#include <cstdio>
#include <string>
#include "pe_convert.h"

int WINAPI WinMain(HINSTANCE hInst, HINSTANCE, LPSTR cmdLine, int nShow) {
    // Resolve paths relative to launcher directory
    char exeDir[MAX_PATH];
    GetModuleFileNameA(NULL, exeDir, MAX_PATH);
    char* lastSlash = strrchr(exeDir, '\\');
    if (lastSlash) *(lastSlash + 1) = '\0';

    std::string originalExe = std::string(exeDir) + "echovr_original.exe";
    std::string gameDll = std::string(exeDir) + "echovr_game.dll";
    std::string entryFile = std::string(exeDir) + "echovr_game.entry";
    std::string patchesDll = std::string(exeDir) + "gamepatches.dll";

    // First-run detection: if echovr_original.exe doesn't exist but echovr_game.dll does,
    // we've already converted. If neither exists, error.
    uint32_t entryRva = 0;

    if (GetFileAttributesA(gameDll.c_str()) == INVALID_FILE_ATTRIBUTES) {
        // Need to convert
        if (GetFileAttributesA(originalExe.c_str()) == INVALID_FILE_ATTRIBUTES) {
            MessageBoxA(NULL, "echovr_original.exe not found.\n\n"
                "Place the original game executable as echovr_original.exe\n"
                "in the same directory as this launcher.",
                "NEVR Launcher", MB_ICONERROR);
            return 1;
        }

        fprintf(stderr, "[NEVR.LAUNCHER] First run: converting echovr_original.exe to DLL...\n");
        if (!ConvertExeToDll(originalExe.c_str(), gameDll.c_str(), &entryRva)) {
            MessageBoxA(NULL, "Failed to convert game executable to DLL.\n"
                "Check console output for details.",
                "NEVR Launcher", MB_ICONERROR);
            return 1;
        }

        // Save entry RVA for future launches
        FILE* f = fopen(entryFile.c_str(), "wb");
        if (f) { fwrite(&entryRva, sizeof(entryRva), 1, f); fclose(f); }

        fprintf(stderr, "[NEVR.LAUNCHER] Conversion complete. Entry RVA: 0x%08X\n", entryRva);
    } else {
        // Load saved entry RVA
        FILE* f = fopen(entryFile.c_str(), "rb");
        if (!f || fread(&entryRva, sizeof(entryRva), 1, f) != 1) {
            MessageBoxA(NULL, "echovr_game.entry missing or corrupt.\n"
                "Delete echovr_game.dll to re-convert.",
                "NEVR Launcher", MB_ICONERROR);
            if (f) fclose(f);
            return 1;
        }
        fclose(f);
    }

    // Step 2: Load the game DLL (maps at preferred base 0x140000000)
    HMODULE hGame = LoadLibraryA(gameDll.c_str());
    if (!hGame) {
        fprintf(stderr, "[NEVR.LAUNCHER] Failed to load echovr_game.dll (error %lu)\n", GetLastError());
        MessageBoxA(NULL, "Failed to load echovr_game.dll.", "NEVR Launcher", MB_ICONERROR);
        return 1;
    }
    fprintf(stderr, "[NEVR.LAUNCHER] Game DLL loaded at 0x%p\n", (void*)hGame);

    // Step 3: Load gamepatches.dll
    HMODULE hPatches = LoadLibraryA(patchesDll.c_str());
    if (!hPatches) {
        fprintf(stderr, "[NEVR.LAUNCHER] Failed to load gamepatches.dll (error %lu)\n", GetLastError());
        // Game can still run without patches
    }

    // Step 4: Tell patches where the game is
    if (hPatches) {
        typedef void (*SetGameModule_fn)(HMODULE);
        auto setGame = (SetGameModule_fn)GetProcAddress(hPatches, "NEVR_SetGameModule");
        if (setGame) {
            setGame(hGame);
            fprintf(stderr, "[NEVR.LAUNCHER] Patches initialized\n");
        } else {
            fprintf(stderr, "[NEVR.LAUNCHER] WARNING: NEVR_SetGameModule not found in gamepatches.dll\n");
        }
    }

    // Step 5: Call game's real entry point
    typedef int (WINAPI *WinMain_fn)(HINSTANCE, HINSTANCE, LPSTR, int);
    auto gameEntry = (WinMain_fn)((char*)hGame + entryRva);

    fprintf(stderr, "[NEVR.LAUNCHER] Calling game entry point at 0x%p\n", (void*)gameEntry);
    return gameEntry(hGame, NULL, GetCommandLineA(), nShow);
}
