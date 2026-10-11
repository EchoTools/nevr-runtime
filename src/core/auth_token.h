/* SYNTHESIS -- custom tool code, not from binary */

#pragma once

#include <nlohmann/json.hpp>
#include <atomic>
#include <string>
#include <cstdint>
#include <cstring>
#include <ctime>
#include <filesystem>
#include <fstream>
#include <optional>
#include <sstream>
#include <vector>

#include "core/logging.h"
#include "core/system_info.h"

#ifdef _WIN32
#ifndef WIN32_LEAN_AND_MEAN
#define WIN32_LEAN_AND_MEAN
#endif
#include <windows.h>
#include <direct.h>
#include <aclapi.h>
#else
#include <sys/stat.h>
#endif

#include "core/auth_token_model.h"

// Get the directory containing the main executable.
// All _local/ paths are resolved relative to this.
inline std::string GetExeDirectory() {
#ifdef _WIN32
    char path[MAX_PATH] = {0};
    GetModuleFileNameA(NULL, path, MAX_PATH);
    // Strip filename, keep directory
    char* last = strrchr(path, '\\');
    if (!last) last = strrchr(path, '/');
    if (last) *(last + 1) = '\0';
    return std::string(path);
#else
    return "";  // Non-Windows: use CWD
#endif
}

// Relative suffixes to search for _local/ from the exe directory (and parents).
static constexpr const char* kLocalSuffixes[] = {
    "_local",
    "..\\_local",
    "..\\..\\_local",
    "../_local",
    "../../_local",
};

inline std::string JoinCredentialPath(const std::string& directory, const std::string& child) {
    if (directory.empty()) return child;
    const char last = directory.back();
    if (last == '/' || last == '\\') return directory + child;
#ifdef _WIN32
    return directory + "\\" + child;
#else
    return directory + "/" + child;
#endif
}

struct CredentialCacheLocation {
    std::string directory;
    std::string credentialsPath;
};

inline bool CredentialPathExists(const std::string& path) {
    std::error_code error;
    return std::filesystem::exists(std::filesystem::path(path), error) && !error;
}

// Select once for both cache reads and writes. Existing credentials take
// precedence over YAML so a refresh cannot silently move an operator's cache;
// absent credentials follow the first NEVR config, then exeDir/_local.
inline CredentialCacheLocation SelectCredentialCacheLocation(const std::string& exeDir) {
    for (const auto* suffix : kLocalSuffixes) {
        const std::string directory = JoinCredentialPath(exeDir, suffix);
        const std::string credentialsPath = JoinCredentialPath(directory, ".credentials.json");
        if (CredentialPathExists(credentialsPath)) return {directory, credentialsPath};
    }

    for (const auto* suffix : kLocalSuffixes) {
        const std::string directory = JoinCredentialPath(exeDir, suffix);
        const std::string configPath = JoinCredentialPath(directory, "config.yaml");
        std::ifstream config(configPath, std::ios::binary);
        if (config.is_open()) {
            return {directory, JoinCredentialPath(directory, ".credentials.json")};
        }
    }

    const std::string directory = JoinCredentialPath(exeDir, "_local");
    return {directory, JoinCredentialPath(directory, ".credentials.json")};
}

// Reads a selected .credentials.json. Missing, unreadable, or malformed files
// return an empty token; no farther cache is consulted after selection.
inline CachedAuthToken LoadCachedAuthTokenFromPath(const std::string& path) {
    std::ifstream file(path, std::ios::binary);
    if (!file.is_open()) return {};

    std::ostringstream contents;
    contents << file.rdbuf();
    if (file.bad()) return {};

    return ParseCredentialsJson(contents.str(), static_cast<uint64_t>(time(nullptr)));
}

inline CachedAuthToken LoadCachedAuthToken(const std::string& exeDir) {
    const CredentialCacheLocation location = SelectCredentialCacheLocation(exeDir);
    return LoadCachedAuthTokenFromPath(location.credentialsPath);
}

// Does NOT validate expiry — caller decides whether to use token or refresh.
inline CachedAuthToken LoadCachedAuthToken() { return LoadCachedAuthToken(GetExeDirectory()); }

// Saves the REFRESH TOKEN (and identity) to _local/.credentials.json.
// The access token is deliberately NOT persisted — it lives in memory only.
// Searches for existing _local/ directory with parent-directory fallback
// (same paths as LoadCachedAuthToken). Creates _local/ next to the executable
// if none found.
inline bool SaveAuthToken(const CachedAuthToken& auth, const std::string& exeDir) {
    if (auth.refresh_token.empty()) return false;

#ifdef _WIN32
    // Wine's Windows ACL readback does not prove the host file's Unix mode.
    // No token bytes may be written until host permissions can be enforced.
    if (nevr_system_info::Get().IsWine()) {
        static std::atomic_flag warned = ATOMIC_FLAG_INIT;
        if (!warned.test_and_set()) {
            Log(EchoVR::LogLevel::Warning,
                "[NEVR.AUTH] Credential cache persistence disabled under Wine: host permissions cannot be verified");
        }
        return false;
    }
#endif

    const CredentialCacheLocation location = SelectCredentialCacheLocation(exeDir);
    const std::string& target_dir = location.directory;
    if (!CredentialPathExists(target_dir)) {
#ifdef _WIN32
        _mkdir(target_dir.c_str());
#else
        mkdir(target_dir.c_str(), 0755);
#endif
    }

    const std::string& path = location.credentialsPath;

#ifdef _WIN32
    HANDLE token = nullptr;
    if (!OpenProcessToken(GetCurrentProcess(), TOKEN_QUERY, &token)) {
        Log(EchoVR::LogLevel::Warning, "[NEVR.AUTH] Cannot inspect current user token: %lu", GetLastError());
        return false;
    }
    DWORD tokenBytes = 0;
    GetTokenInformation(token, TokenUser, nullptr, 0, &tokenBytes);
    std::vector<BYTE> tokenInfo(tokenBytes);
    const bool haveUser = tokenBytes != 0 &&
        GetTokenInformation(token, TokenUser, tokenInfo.data(), tokenBytes, &tokenBytes);
    const DWORD userError = haveUser ? ERROR_SUCCESS : GetLastError();
    CloseHandle(token);
    if (!haveUser) {
        Log(EchoVR::LogLevel::Warning, "[NEVR.AUTH] Cannot read current user SID: %lu", userError);
        return false;
    }

    const auto* user = reinterpret_cast<const TOKEN_USER*>(tokenInfo.data());
    EXPLICIT_ACCESSA access = {};
    access.grfAccessPermissions = GENERIC_ALL;
    access.grfAccessMode = SET_ACCESS;
    access.grfInheritance = NO_INHERITANCE;
    access.Trustee.TrusteeForm = TRUSTEE_IS_SID;
    access.Trustee.ptstrName = reinterpret_cast<LPSTR>(user->User.Sid);
    PACL acl = nullptr;
    const DWORD aclError = SetEntriesInAclA(1, &access, nullptr, &acl);
    if (aclError != ERROR_SUCCESS) {
        Log(EchoVR::LogLevel::Warning, "[NEVR.AUTH] Cannot build credentials DACL: %lu", aclError);
        return false;
    }

    SECURITY_DESCRIPTOR descriptor = {};
    if (!InitializeSecurityDescriptor(&descriptor, SECURITY_DESCRIPTOR_REVISION) ||
        !SetSecurityDescriptorDacl(&descriptor, TRUE, acl, FALSE) ||
        !SetSecurityDescriptorControl(&descriptor, SE_DACL_PROTECTED, SE_DACL_PROTECTED)) {
        const DWORD error = GetLastError();
        LocalFree(acl);
        Log(EchoVR::LogLevel::Warning, "[NEVR.AUTH] Cannot prepare credentials security descriptor: %lu", error);
        return false;
    }
    SECURITY_ATTRIBUTES attributes = {sizeof(attributes), &descriptor, FALSE};
    HANDLE file = CreateFileA(path.c_str(), GENERIC_WRITE | WRITE_DAC | READ_CONTROL, 0, &attributes,
        OPEN_ALWAYS, FILE_ATTRIBUTE_HIDDEN, nullptr);
    if (file == INVALID_HANDLE_VALUE) {
        const DWORD error = GetLastError();
        LocalFree(acl);
        Log(EchoVR::LogLevel::Warning, "[NEVR.AUTH] Failed to open %s for writing: %lu", path.c_str(), error);
        return false;
    }
    const bool newlyCreated = GetLastError() != ERROR_ALREADY_EXISTS;

    // OPEN_ALWAYS does not replace an existing file's DACL. Protect it before
    // truncating or writing, then read it back. Wine can report success here
    // without enforcing the requested ACL on its host file.
    const DWORD protectError = SetSecurityInfo(file, SE_FILE_OBJECT,
        DACL_SECURITY_INFORMATION | PROTECTED_DACL_SECURITY_INFORMATION,
        nullptr, nullptr, acl, nullptr);
    LocalFree(acl);
    PSECURITY_DESCRIPTOR observed = nullptr;
    PACL observedAcl = nullptr;
    const DWORD readError = protectError == ERROR_SUCCESS
        ? GetSecurityInfo(file, SE_FILE_OBJECT, DACL_SECURITY_INFORMATION,
              nullptr, nullptr, &observedAcl, nullptr, &observed)
        : protectError;
    SECURITY_DESCRIPTOR_CONTROL control = 0;
    DWORD revision = 0;
    void* ace = nullptr;
    const bool protectedForUser = readError == ERROR_SUCCESS && observed != nullptr &&
        observedAcl != nullptr && GetSecurityDescriptorControl(observed, &control, &revision) &&
        (control & SE_DACL_PROTECTED) != 0 && observedAcl->AceCount == 1 &&
        GetAce(observedAcl, 0, &ace) &&
        static_cast<ACE_HEADER*>(ace)->AceType == ACCESS_ALLOWED_ACE_TYPE &&
        EqualSid(user->User.Sid, &static_cast<ACCESS_ALLOWED_ACE*>(ace)->SidStart);
    if (observed != nullptr) LocalFree(observed);
    if (!protectedForUser) {
        CloseHandle(file);
        if (newlyCreated) DeleteFileA(path.c_str());
        if (readError == ERROR_SUCCESS) {
            Log(EchoVR::LogLevel::Warning,
                "[NEVR.AUTH] User-only DACL was not preserved for %s; credentials not written",
                path.c_str());
        } else {
            Log(EchoVR::LogLevel::Warning,
                "[NEVR.AUTH] Cannot verify DACL for %s before writing: %lu",
                path.c_str(), readError);
        }
        return false;
    }

    LARGE_INTEGER start = {};
    bool written = SetFilePointerEx(file, start, nullptr, FILE_BEGIN) && SetEndOfFile(file);
    if (written) {
        const std::string json = SerializeCredentialsJson(auth);
        size_t offset = 0;
        while (offset < json.size()) {
            const size_t remaining = json.size() - offset;
            const DWORD count = remaining > MAXDWORD ? MAXDWORD : static_cast<DWORD>(remaining);
            DWORD actual = 0;
            if (!WriteFile(file, json.data() + offset, count, &actual, nullptr) || actual == 0) {
                written = false;
                break;
            }
            offset += actual;
        }
    }
    const DWORD writeError = written ? ERROR_SUCCESS : GetLastError();
    const bool closed = CloseHandle(file);
    if (!written || !closed) {
        Log(EchoVR::LogLevel::Warning, "[NEVR.AUTH] Failed to write %s: %lu",
            path.c_str(), written ? GetLastError() : writeError);
        return false;
    }
    if (!SetFileAttributesA(path.c_str(), FILE_ATTRIBUTE_HIDDEN)) {
        Log(EchoVR::LogLevel::Warning,
            "[NEVR.AUTH] SetFileAttributesA failed for %s: %lu — credentials file left visible",
            path.c_str(), GetLastError());
    }
#else
    // Set restrictive umask before creating file so it's never world-readable.
    mode_t old_umask = umask(0177);
    std::ofstream out(path, std::ios::trunc);
    if (!out.is_open()) {
        // Was a raw fprintf(stderr, ...) — Hard Stop violation (CPP-MINGW-ADDENDUM
        // "No printf"; docs/standards/logging.md Rule 6, Log() is the single entry
        // point). Log() is already in scope in this TU (see the includes above).
        Log(EchoVR::LogLevel::Warning, "[NEVR.AUTH] Failed to open %s for writing",
            path.c_str());
        umask(old_umask);
        return false;
    }

    // Access token deliberately NOT written (see SerializeCredentialsJson).
    out << SerializeCredentialsJson(auth);
    out.close();

    umask(old_umask);
#endif

    return true;
}

inline bool SaveAuthToken(const CachedAuthToken& auth) { return SaveAuthToken(auth, GetExeDirectory()); }
