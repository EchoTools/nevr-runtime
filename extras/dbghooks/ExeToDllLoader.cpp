#include <windows.h>
#include <stdio.h>

// =============================================================================
// EXE-to-DLL Loader for Echo VR
// =============================================================================
// This converts echovr.exe into a DLL on-the-fly and loads it, allowing us to
// install hooks BEFORE the game's static initializers run.
//
// Strategy:
// 1. Load echovr.exe as data (not executable)
// 2. Patch PE header: change IMAGE_FILE_EXECUTABLE_IMAGE to IMAGE_FILE_DLL
// 3. Write modified image to temporary DLL file
// 4. Install our hooks (DbgHooks.dll)
// 5. LoadLibrary the modified DLL
// 6. Call the DLL's entry point (original WinMain)
// =============================================================================

void Log(const char* format, ...) {
    va_list args;
    va_start(args, format);
    printf("[ExeToDLL] ");
    vprintf(format, args);
    printf("\n");
    va_end(args);
}

void PrintError(const char* msg) {
    DWORD error = GetLastError();
    Log("ERROR: %s (code: %d)", msg, error);
}

bool ConvertExeToDll(const char* exePath, const char* dllPath) {
    Log("Reading EXE: %s", exePath);
    
    // Read entire EXE into memory
    HANDLE hFile = CreateFileA(exePath, GENERIC_READ, FILE_SHARE_READ, 
                               nullptr, OPEN_EXISTING, FILE_ATTRIBUTE_NORMAL, nullptr);
    if (hFile == INVALID_HANDLE_VALUE) {
        PrintError("Failed to open EXE");
        return false;
    }
    
    DWORD fileSize = GetFileSize(hFile, nullptr);
    BYTE* fileData = new BYTE[fileSize];
    
    DWORD bytesRead;
    if (!ReadFile(hFile, fileData, fileSize, &bytesRead, nullptr)) {
        PrintError("Failed to read EXE");
        CloseHandle(hFile);
        delete[] fileData;
        return false;
    }
    CloseHandle(hFile);
    
    Log("EXE size: %d bytes", fileSize);
    
    // Parse PE header
    IMAGE_DOS_HEADER* dosHeader = (IMAGE_DOS_HEADER*)fileData;
    if (dosHeader->e_magic != IMAGE_DOS_SIGNATURE) {
        Log("ERROR: Invalid DOS signature");
        delete[] fileData;
        return false;
    }
    
    IMAGE_NT_HEADERS* ntHeaders = (IMAGE_NT_HEADERS*)(fileData + dosHeader->e_lfanew);
    if (ntHeaders->Signature != IMAGE_NT_SIGNATURE) {
        Log("ERROR: Invalid PE signature");
        delete[] fileData;
        return false;
    }
    
    Log("Original characteristics: 0x%04X", ntHeaders->FileHeader.Characteristics);
    
    // Patch: Convert EXE to DLL
    // Remove IMAGE_FILE_EXECUTABLE_IMAGE flag and add IMAGE_FILE_DLL flag
    ntHeaders->FileHeader.Characteristics &= ~IMAGE_FILE_EXECUTABLE_IMAGE;
    ntHeaders->FileHeader.Characteristics |= IMAGE_FILE_DLL;
    
    Log("Patched characteristics: 0x%04X", ntHeaders->FileHeader.Characteristics);
    
    // Write modified image to DLL file
    hFile = CreateFileA(dllPath, GENERIC_WRITE, 0, nullptr, 
                       CREATE_ALWAYS, FILE_ATTRIBUTE_NORMAL, nullptr);
    if (hFile == INVALID_HANDLE_VALUE) {
        PrintError("Failed to create DLL");
        delete[] fileData;
        return false;
    }
    
    DWORD bytesWritten;
    if (!WriteFile(hFile, fileData, fileSize, &bytesWritten, nullptr)) {
        PrintError("Failed to write DLL");
        CloseHandle(hFile);
        delete[] fileData;
        return false;
    }
    
    CloseHandle(hFile);
    delete[] fileData;
    
    Log("DLL created: %s", dllPath);
    return true;
}

int main(int argc, char* argv[]) {
    printf("=============================================================================\n");
    printf("Echo VR EXE-to-DLL Loader\n");
    printf("Converts echovr.exe to DLL and loads with hooks installed first\n");
    printf("=============================================================================\n\n");
    
    const char* exePath = (argc > 1) ? argv[1] : "echovr.exe";
    const char* hooksDll = (argc > 2) ? argv[2] : "DbgHooks.dll";
    const char* tempDll = "echovr_temp.dll";
    
    Log("EXE path: %s", exePath);
    Log("Hooks DLL: %s", hooksDll);
    Log("Temp DLL: %s", tempDll);
    printf("\n");
    
    // Step 1: Convert EXE to DLL
    if (!ConvertExeToDll(exePath, tempDll)) {
        Log("Failed to convert EXE to DLL");
        return 1;
    }
    printf("\n");
    
    // Step 2: Load hooks DLL FIRST (before game's static initializers)
    Log("Loading hooks DLL: %s", hooksDll);
    HMODULE hHooks = LoadLibraryA(hooksDll);
    if (!hHooks) {
        PrintError("Failed to load hooks DLL");
        return 1;
    }
    Log("Hooks loaded at: %p", hHooks);
    printf("\n");
    
    // Step 3: Load game DLL (now hooks are installed!)
    Log("Loading game DLL: %s", tempDll);
    HMODULE hGame = LoadLibraryA(tempDll);
    if (!hGame) {
        PrintError("Failed to load game DLL");
        return 1;
    }
    Log("Game loaded at: %p", hGame);
    printf("\n");
    
    // Step 4: Find and call the entry point
    // For a DLL converted from EXE, the entry point is still WinMain
    // We need to find it by parsing exports or use a known address
    Log("Game is running as DLL");
    Log("Note: Game may not run correctly as DLL without proper entry point handling");
    Log("This is a proof-of-concept for early hook installation timing");
    printf("\n");
    
    // Wait for user to close
    Log("Press Enter to unload...");
    getchar();
    
    // Cleanup
    if (hGame) FreeLibrary(hGame);
    if (hHooks) FreeLibrary(hHooks);
    DeleteFileA(tempDll);
    
    return 0;
}
