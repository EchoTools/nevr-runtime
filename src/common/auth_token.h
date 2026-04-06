/* SYNTHESIS -- custom tool code, not from binary */

#pragma once

#include <nlohmann/json.hpp>
#include <string>
#include <cstdint>
#include <cstring>
#include <ctime>
#include <fstream>
#include <sstream>
#include <vector>

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

// Reads _local/.credentials.json relative to the executable, with parent-directory fallback.
// Returns empty token on missing file, parse failure, or malformed data.
// Does NOT validate expiry — caller decides whether to use token or refresh.
inline CachedAuthToken LoadCachedAuthToken() {
    std::string contents;
    std::string exeDir = GetExeDirectory();
    for (const auto* suffix : kLocalSuffixes) {
        std::string path = exeDir + suffix + "/.credentials.json";
        std::ifstream f(path, std::ios::binary);
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

// Saves auth token to _local/.credentials.json. Searches for existing _local/ directory
// with parent-directory fallback (same paths as LoadCachedAuthToken). Creates
// _local/ next to the executable if none found.
inline bool SaveAuthToken(const CachedAuthToken& auth) {
    if (auth.token.empty()) return false;

    // Find existing _local/ dir relative to executable
    std::string exeDir = GetExeDirectory();
    std::string target_dir;
    for (const auto* suffix : kLocalSuffixes) {
        std::string probe = exeDir + suffix + "/config.json";
        if (std::ifstream(probe).is_open()) {
            target_dir = exeDir + suffix;
            break;
        }
    }
    if (target_dir.empty()) {
        target_dir = exeDir + "_local";
#ifdef _WIN32
        _mkdir(target_dir.c_str());
#endif
    }

    std::string path = target_dir + "/.credentials.json";

#ifndef _WIN32
    // Set restrictive umask before creating file so it's never world-readable
    mode_t old_umask = umask(0177);
#endif

    std::ofstream out(path, std::ios::trunc);
    if (!out.is_open()) {
#ifndef _WIN32
        umask(old_umask);
#endif
        return false;
    }

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
    // Hide the file and restrict to current user only
    SetFileAttributesA(path.c_str(), FILE_ATTRIBUTE_HIDDEN);

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
                    SetNamedSecurityInfoA(const_cast<LPSTR>(path.c_str()),
                        SE_FILE_OBJECT, DACL_SECURITY_INFORMATION | PROTECTED_DACL_SECURITY_INFORMATION,
                        nullptr, nullptr, pAcl, nullptr);
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
