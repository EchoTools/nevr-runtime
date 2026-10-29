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
#include "core/credential_cache_stats.h"
#include "core/credential_write.h"
#include "core/nevr_root.h"

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

// The nevr root of this Windows user (core/nevr_root.h); empty off Windows or without %LOCALAPPDATA%.
// The writer below takes ANSI paths, so a root that does not survive the conversion (a user name outside the ANSI code page) is
// no root: the cache then stays where the older search finds it, which works, instead of being lost to a path with '?' in it.
inline std::string DefaultCredentialRoot() {
#ifdef _WIN32
    const std::wstring wide = nevr_root::RootW();
    if (wide.empty()) return std::string();
    BOOL lossy = FALSE;
    const int needed = WideCharToMultiByte(CP_ACP, 0, wide.c_str(), static_cast<int>(wide.size()), nullptr, 0, nullptr, &lossy);
    if (needed <= 0 || lossy != FALSE) return std::string();
    std::string ansi(static_cast<std::size_t>(needed), '\0');
    WideCharToMultiByte(CP_ACP, 0, wide.c_str(), static_cast<int>(wide.size()), ansi.data(), needed, nullptr, &lossy);
    return lossy == FALSE ? ansi : std::string();
#else
    return std::string();
#endif
}

// The cache before the nevr root existed. Existing credentials take precedence over YAML so a refresh cannot silently move
// an operator's cache; absent credentials follow the first NEVR config, then exeDir/_local.
inline CredentialCacheLocation SelectLegacyCredentialCacheLocation(const std::string& exeDir) {
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

// Select once for both cache reads and writes (ADR 0012): the sign-in cache is one file in the nevr root. A cache the one-time
// move could not move (nevr_root_move) is still read, and written back where it is, until a later run moves it; the root's
// own cache always wins. Without a root (no %LOCALAPPDATA%) the legacy search above decides.
inline CredentialCacheLocation SelectCredentialCacheLocation(const std::string& exeDir, const std::string& root) {
    if (root.empty()) return SelectLegacyCredentialCacheLocation(exeDir);
    const std::string rootPath = nevr_root::CredentialsPath(root);
    if (CredentialPathExists(rootPath)) return {root, rootPath};
    for (const auto* suffix : kLocalSuffixes) {
        const std::string directory = JoinCredentialPath(exeDir, suffix);
        const std::string credentialsPath = JoinCredentialPath(directory, ".credentials.json");
        if (CredentialPathExists(credentialsPath)) return {directory, credentialsPath};
    }
    return {root, rootPath};
}

inline CredentialCacheLocation SelectCredentialCacheLocation(const std::string& exeDir) {
    return SelectCredentialCacheLocation(exeDir, DefaultCredentialRoot());
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

inline CachedAuthToken LoadCachedAuthToken(const std::string& exeDir, const std::string& root) {
    const CredentialCacheLocation location = SelectCredentialCacheLocation(exeDir, root);
    return LoadCachedAuthTokenFromPath(location.credentialsPath);
}

inline CachedAuthToken LoadCachedAuthToken(const std::string& exeDir) {
    return LoadCachedAuthToken(exeDir, DefaultCredentialRoot());
}

// Does NOT validate expiry — caller decides whether to use token or refresh.
inline CachedAuthToken LoadCachedAuthToken() { return LoadCachedAuthToken(GetExeDirectory()); }

// Saves the REFRESH TOKEN (and identity) to .credentials.json in the place SelectCredentialCacheLocation picks: the nevr
// root, created when missing. The access token is deliberately NOT persisted — it lives in memory only.
inline bool SaveAuthToken(const CachedAuthToken& auth, const std::string& exeDir, const std::string& root) {
    if (auth.refresh_token.empty()) return false;

    const CredentialCacheLocation location = SelectCredentialCacheLocation(exeDir, root);
    const std::string& target_dir = location.directory;
    if (!CredentialPathExists(target_dir)) {
#ifdef _WIN32
        nevr_root::EnsureDirectoryTree(target_dir);  // a failure shows as the temp file's write failing, with its reason
#else
        mkdir(target_dir.c_str(), 0755);
#endif
    }

    const std::string& path = location.credentialsPath;
    const std::string tempPath = nevr_credential_write::TempPathFor(path);
#ifdef _WIN32
    const bool isWine = nevr_system_info::Get().IsWine();
#else
    const bool isWine = false;
#endif

    // Access token deliberately NOT written (see SerializeCredentialsJson). The token is protected, written
    // and flushed in a temp file, then renamed over the cache; any failure leaves the previous cache as it was
    // (core/credential_write.h).
    std::string why;
    if (!nevr_credential_write::WriteProtectedTemp(tempPath, SerializeCredentialsJson(auth), isWine, &why)) {
        Log(EchoVR::LogLevel::Warning,
            "[NEVR.AUTH] Credentials not written to %s: %s; the previous cache is unchanged%s", path.c_str(),
            why.c_str(), isWine ? " (Wine: host permissions could not be set and verified)" : "");
        nevr_credential_stats::NoteRefused(why);
        return false;
    }
    if (!nevr_credential_write::ReplaceCache(tempPath, path, &why)) {
        Log(EchoVR::LogLevel::Warning, "[NEVR.AUTH] Credentials not written to %s: %s; the previous cache is unchanged",
            path.c_str(), why.c_str());
        nevr_credential_stats::NoteRefused(why);
        return false;
    }
    // The record that a launch persisted its sign-in (#562): the path and how the file is protected, never a token
    // byte or a length. Under Wine the host mode was read back as 600 before the write and again before the replace.
#ifdef _WIN32
    const char* const protection = isWine ? "host_mode_600" : "dacl";
#else
    const char* const protection = "mode_0600";
#endif
    Log(EchoVR::LogLevel::Info, "[NEVR.AUTH] Credentials written to %s protection=%s", path.c_str(), protection);
    nevr_credential_stats::NoteSaved(protection);
    return true;
}

inline bool SaveAuthToken(const CachedAuthToken& auth, const std::string& exeDir) {
    return SaveAuthToken(auth, exeDir, DefaultCredentialRoot());
}

inline bool SaveAuthToken(const CachedAuthToken& auth) { return SaveAuthToken(auth, GetExeDirectory()); }
