#include "pe_convert.h"

#include <windows.h>
#include <cstdarg>
#include <cstdio>
#include <cstring>
#include <vector>

static void LogPE(const char* fmt, ...) {
    va_list args;
    va_start(args, fmt);
    fprintf(stderr, "[NEVR.PE] ");
    vfprintf(stderr, fmt, args);
    fprintf(stderr, "\n");
    va_end(args);
}

bool ConvertExeToDll(const char* exe_path, const char* dll_path, uint32_t* out_entry_rva) {
    // Step 1: Read entire PE into memory
    HANDLE hFile = CreateFileA(exe_path, GENERIC_READ, FILE_SHARE_READ,
                               nullptr, OPEN_EXISTING, FILE_ATTRIBUTE_NORMAL, nullptr);
    if (hFile == INVALID_HANDLE_VALUE) {
        LogPE("Failed to open %s (error %lu)", exe_path, GetLastError());
        return false;
    }

    DWORD fileSize = GetFileSize(hFile, nullptr);
    if (fileSize == INVALID_FILE_SIZE || fileSize == 0) {
        LogPE("Failed to get file size for %s", exe_path);
        CloseHandle(hFile);
        return false;
    }

    std::vector<BYTE> fileData(fileSize);
    DWORD bytesRead;
    if (!ReadFile(hFile, fileData.data(), fileSize, &bytesRead, nullptr) || bytesRead != fileSize) {
        LogPE("Failed to read %s (error %lu)", exe_path, GetLastError());
        CloseHandle(hFile);
        return false;
    }
    CloseHandle(hFile);

    LogPE("Read %s (%lu bytes)", exe_path, fileSize);

    // Step 2: Validate DOS header
    if (fileSize < sizeof(IMAGE_DOS_HEADER)) {
        LogPE("File too small for DOS header");
        return false;
    }

    auto* dosHeader = reinterpret_cast<IMAGE_DOS_HEADER*>(fileData.data());
    if (dosHeader->e_magic != 0x5A4D) { // MZ
        LogPE("Invalid DOS signature: 0x%04X (expected 0x5A4D)", dosHeader->e_magic);
        return false;
    }

    // Validate NT headers offset is within bounds
    if (dosHeader->e_lfanew < 0 ||
        static_cast<DWORD>(dosHeader->e_lfanew) + sizeof(IMAGE_NT_HEADERS64) > fileSize) {
        LogPE("Invalid e_lfanew offset: 0x%lX", dosHeader->e_lfanew);
        return false;
    }

    // Step 2b: Validate NT signature
    auto* ntHeaders = reinterpret_cast<IMAGE_NT_HEADERS64*>(fileData.data() + dosHeader->e_lfanew);
    if (ntHeaders->Signature != 0x4550) { // PE\0\0
        LogPE("Invalid PE signature: 0x%08lX (expected 0x00004550)", ntHeaders->Signature);
        return false;
    }

    // Verify this is a 64-bit PE
    if (ntHeaders->OptionalHeader.Magic != IMAGE_NT_OPTIONAL_HDR64_MAGIC) {
        LogPE("Not a 64-bit PE (Magic=0x%04X, expected 0x%04X)",
              ntHeaders->OptionalHeader.Magic, IMAGE_NT_OPTIONAL_HDR64_MAGIC);
        return false;
    }

    LogPE("Original characteristics: 0x%04X", ntHeaders->FileHeader.Characteristics);
    LogPE("Original entry point RVA: 0x%08lX", ntHeaders->OptionalHeader.AddressOfEntryPoint);

    // Step 3: Save original AddressOfEntryPoint
    *out_entry_rva = ntHeaders->OptionalHeader.AddressOfEntryPoint;

    // Step 4: Flip characteristics — clear EXE, set DLL
    ntHeaders->FileHeader.Characteristics &= ~IMAGE_FILE_EXECUTABLE_IMAGE; // clear 0x0002
    ntHeaders->FileHeader.Characteristics |= IMAGE_FILE_DLL;              // set 0x2000

    LogPE("Patched characteristics: 0x%04X", ntHeaders->FileHeader.Characteristics);

    // Step 5: Write stub DllMain into padding at end of last executable section
    // Stub: B8 01 00 00 00 C3  (mov eax, 1 / ret)
    static const BYTE stub[] = { 0xB8, 0x01, 0x00, 0x00, 0x00, 0xC3 };
    constexpr DWORD stubSize = sizeof(stub);

    // Find section headers
    auto* sectionHeaders = reinterpret_cast<IMAGE_SECTION_HEADER*>(
        reinterpret_cast<BYTE*>(&ntHeaders->OptionalHeader) +
        ntHeaders->FileHeader.SizeOfOptionalHeader);
    WORD numSections = ntHeaders->FileHeader.NumberOfSections;

    // Find the last section with executable characteristics (.text)
    IMAGE_SECTION_HEADER* execSection = nullptr;
    for (WORD i = 0; i < numSections; i++) {
        if (sectionHeaders[i].Characteristics & IMAGE_SCN_MEM_EXECUTE) {
            execSection = &sectionHeaders[i];
        }
    }

    if (!execSection) {
        LogPE("No executable section found");
        return false;
    }

    LogPE("Executable section: %.8s (VA=0x%08lX, VSize=0x%08lX, RawOff=0x%08lX, RawSize=0x%08lX)",
          execSection->Name,
          execSection->VirtualAddress,
          execSection->Misc.VirtualSize,
          execSection->PointerToRawData,
          execSection->SizeOfRawData);

    // Compute where the actual data ends within this section's raw data
    // VirtualSize is the actual used size; SizeOfRawData is the file-aligned size
    DWORD usedEnd = execSection->PointerToRawData + execSection->Misc.VirtualSize;
    DWORD rawEnd = execSection->PointerToRawData + execSection->SizeOfRawData;

    if (usedEnd + stubSize > rawEnd) {
        LogPE("Not enough padding in executable section for stub "
              "(need %lu bytes, have %lu bytes)",
              stubSize, rawEnd - usedEnd);
        return false;
    }

    if (usedEnd + stubSize > fileSize) {
        LogPE("Stub offset 0x%08lX exceeds file size 0x%08lX", usedEnd, fileSize);
        return false;
    }

    // Write the stub into the padding
    memcpy(fileData.data() + usedEnd, stub, stubSize);

    // Compute the RVA of the stub: section VA + offset within section
    DWORD stubRva = execSection->VirtualAddress + execSection->Misc.VirtualSize;
    ntHeaders->OptionalHeader.AddressOfEntryPoint = stubRva;

    LogPE("Stub DllMain written at file offset 0x%08lX (RVA 0x%08lX)", usedEnd, stubRva);

    // Step 7: Write the patched PE as the DLL
    HANDLE hOut = CreateFileA(dll_path, GENERIC_WRITE, 0,
                              nullptr, CREATE_ALWAYS, FILE_ATTRIBUTE_NORMAL, nullptr);
    if (hOut == INVALID_HANDLE_VALUE) {
        LogPE("Failed to create %s (error %lu)", dll_path, GetLastError());
        return false;
    }

    DWORD bytesWritten;
    if (!WriteFile(hOut, fileData.data(), fileSize, &bytesWritten, nullptr) ||
        bytesWritten != fileSize) {
        LogPE("Failed to write %s (error %lu)", dll_path, GetLastError());
        CloseHandle(hOut);
        return false;
    }
    CloseHandle(hOut);

    LogPE("DLL written: %s (%lu bytes)", dll_path, fileSize);
    LogPE("Original entry RVA: 0x%08lX, stub entry RVA: 0x%08lX", *out_entry_rva, stubRva);

    return true;
}
