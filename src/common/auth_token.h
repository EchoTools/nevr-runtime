/* SYNTHESIS -- custom tool code, not from binary */

#pragma once

#include <nlohmann/json.hpp>
#include <string>
#include <cstdint>
#include <ctime>
#include <fstream>
#include <sstream>

#ifdef _WIN32
#ifndef WIN32_LEAN_AND_MEAN
#define WIN32_LEAN_AND_MEAN
#endif
#include <windows.h>
#include <direct.h>
#endif

struct CachedAuthToken {
    std::string token;
    uint64_t token_expiry = 0;
    std::string refresh_token;
    uint64_t refresh_token_expiry = 0;
    std::string user_id;
    std::string username;

    bool HasValidToken() const {
        return !token.empty() && token_expiry > static_cast<uint64_t>(time(nullptr)) + 60;
    }

    bool HasValidRefreshToken() const {
        return !refresh_token.empty() && refresh_token_expiry > static_cast<uint64_t>(time(nullptr)) + 60;
    }
};

// Search paths for _local/auth.json (supports nested directory layouts).
static const char* const kAuthJsonPaths[] = {
    "_local/auth.json",
    "..\\_local\\auth.json",
    "..\\..\\_local\\auth.json",
    "../_local/auth.json",
    "../../_local/auth.json",
};

// Search paths for _local/ directory (for saving).
static const char* const kLocalDirProbes[] = {
    "_local/config.json",
    "..\\_local\\config.json",
    "..\\..\\_local\\config.json",
    "../_local/config.json",
    "../../_local/config.json",
};

// Corresponding directory paths for each probe.
static const char* const kLocalDirs[] = {
    "_local",
    "..\\_local",
    "..\\..\\_local",
    "../_local",
    "../../_local",
};

// Reads _local/auth.json with parent-directory fallback.
// Returns empty token on missing file, parse failure, or malformed data.
// Does NOT validate expiry — caller decides whether to use token or refresh.
inline CachedAuthToken LoadCachedAuthToken() {
    std::string contents;
    for (const auto* p : kAuthJsonPaths) {
        std::ifstream f(p, std::ios::binary);
        if (f.is_open()) {
            std::ostringstream ss;
            ss << f.rdbuf();
            contents = ss.str();
            break;
        }
    }
    if (contents.empty()) return {};

    try {
        auto j = nlohmann::json::parse(contents);
        CachedAuthToken result;
        result.token = j.value("token", "");
        result.token_expiry = j.value("token_expiry", uint64_t(0));
        // Backwards compat: old format used "expiry" for token expiry
        if (result.token_expiry == 0)
            result.token_expiry = j.value("expiry", uint64_t(0));
        result.refresh_token = j.value("refresh_token", "");
        result.refresh_token_expiry = j.value("refresh_token_expiry", uint64_t(0));
        result.user_id = j.value("user_id", "");
        result.username = j.value("username", "");
        return result;
    } catch (...) {
        return {};
    }
}

// Saves auth token to _local/auth.json. Searches for existing _local/ directory
// with parent-directory fallback (same paths as LoadCachedAuthToken). Creates
// _local/ next to the executable if none found.
inline bool SaveAuthToken(const CachedAuthToken& auth) {
    if (auth.token.empty()) return false;

    // Find existing _local/ dir
    std::string target_dir;
    for (size_t i = 0; i < sizeof(kLocalDirProbes) / sizeof(kLocalDirProbes[0]); ++i) {
        if (std::ifstream(kLocalDirProbes[i]).is_open()) {
            target_dir = kLocalDirs[i];
            break;
        }
    }
    if (target_dir.empty()) {
#ifdef _WIN32
        _mkdir("_local");
#endif
        target_dir = "_local";
    }

    std::string path = target_dir + "/auth.json";
    std::ofstream out(path, std::ios::trunc);
    if (!out.is_open()) return false;

    nlohmann::json j;
    j["token"] = auth.token;
    j["token_expiry"] = auth.token_expiry;
    if (!auth.refresh_token.empty()) {
        j["refresh_token"] = auth.refresh_token;
        j["refresh_token_expiry"] = auth.refresh_token_expiry;
    }
    if (!auth.user_id.empty()) j["user_id"] = auth.user_id;
    if (!auth.username.empty()) j["username"] = auth.username;

    out << j.dump(2) << "\n";
    out.close();

#ifdef _WIN32
    // Hide the file and restrict permissions
    SetFileAttributesA(path.c_str(), FILE_ATTRIBUTE_HIDDEN);
#endif

    return true;
}
