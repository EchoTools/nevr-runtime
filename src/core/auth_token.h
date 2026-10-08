/* SYNTHESIS -- custom tool code, not from binary */

#pragma once

#include <nlohmann/json.hpp>
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

#ifndef _WIN32
    // Set restrictive umask before creating file so it's never world-readable
    mode_t old_umask = umask(0177);
#endif

    std::ofstream out(path, std::ios::trunc);
    if (!out.is_open()) {
        // Was a raw fprintf(stderr, ...) — Hard Stop violation (CPP-MINGW-ADDENDUM
        // "No printf"; docs/standards/logging.md Rule 6, Log() is the single entry
        // point). Log() is already in scope in this TU (see the includes above).
        Log(EchoVR::LogLevel::Warning, "[NEVR.AUTH] Failed to open %s for writing",
            path.c_str());
#ifndef _WIN32
        umask(old_umask);
#endif
        return false;
    }

    // Access token deliberately NOT written (see SerializeCredentialsJson).
    out << SerializeCredentialsJson(auth);
    out.close();

#ifdef _WIN32
    // Hide the file and restrict to current user only
    if (!SetFileAttributesA(path.c_str(), FILE_ATTRIBUTE_HIDDEN)) {
        Log(EchoVR::LogLevel::Warning,
            "[NEVR.AUTH] SetFileAttributesA failed for %s: %lu — credentials file left "
            "visible (not hidden); not a security issue on its own",
            path.c_str(), GetLastError());
    }

    // Set restrictive DACL: only current user gets full access
    HANDLE hToken = nullptr;
    if (OpenProcessToken(GetCurrentProcess(), TOKEN_QUERY, &hToken)) {
        DWORD len = 0;
        GetTokenInformation(hToken, TokenUser, nullptr, 0, &len);
        if (len > 0) {
            std::vector<BYTE> buf(len);
            if (GetTokenInformation(hToken, TokenUser, buf.data(), len, &len)) {
                TOKEN_USER* pUser = reinterpret_cast<TOKEN_USER*>(buf.data());
                EXPLICIT_ACCESSA ea = {};
                ea.grfAccessPermissions = GENERIC_ALL;
                ea.grfAccessMode = SET_ACCESS;
                ea.grfInheritance = NO_INHERITANCE;
                ea.Trustee.TrusteeForm = TRUSTEE_IS_SID;
                ea.Trustee.ptstrName = reinterpret_cast<LPSTR>(pUser->User.Sid);
                PACL pAcl = nullptr;
                if (SetEntriesInAclA(1, &ea, nullptr, &pAcl) == ERROR_SUCCESS) {
                    DWORD aclErr = SetNamedSecurityInfoA(const_cast<LPSTR>(path.c_str()),
                        SE_FILE_OBJECT, DACL_SECURITY_INFORMATION | PROTECTED_DACL_SECURITY_INFORMATION,
                        nullptr, nullptr, pAcl, nullptr);
                    if (aclErr != ERROR_SUCCESS) {
                        Log(EchoVR::LogLevel::Warning,
                            "[NEVR.AUTH] SetNamedSecurityInfoA failed for %s: %lu — DACL "
                            "restriction NOT applied; credentials file may be readable by "
                            "other users on this machine",
                            path.c_str(), aclErr);
                    }
                    LocalFree(pAcl);
                }
            }
        }
        CloseHandle(hToken);
    }
#else
    umask(old_umask);
#endif

    return true;
}

inline bool SaveAuthToken(const CachedAuthToken& auth) { return SaveAuthToken(auth, GetExeDirectory()); }
