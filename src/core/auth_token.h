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

    // Extracts the "did" (discord ID) claim from the JWT access token.
    // Returns 0 if the token is missing, malformed, or has no "did" claim.
    /// Decode the JWT payload (segment 2) into its claims object.
    /// Factored out of GetDiscordId so expiry can reuse it — the base64url
    /// decode was otherwise about to exist twice.
    /// Returns an empty object if the token is absent or malformed.
    nlohmann::json DecodeClaims() const {
        if (token.empty()) return nlohmann::json::object();
        // JWT = header.payload.signature — decode the payload (second segment)
        auto dot1 = token.find('.');
        if (dot1 == std::string::npos) return nlohmann::json::object();
        auto dot2 = token.find('.', dot1 + 1);
        if (dot2 == std::string::npos) return nlohmann::json::object();
        std::string encoded = token.substr(dot1 + 1, dot2 - dot1 - 1);
        // Base64url decode (pad to multiple of 4)
        for (auto& c : encoded) { if (c == '-') c = '+'; if (c == '_') c = '/'; }
        while (encoded.size() % 4) encoded += '=';
        static const std::string b64 = "ABCDEFGHIJKLMNOPQRSTUVWXYZabcdefghijklmnopqrstuvwxyz0123456789+/";
        std::string decoded;
        uint32_t buf = 0; int bits = 0;
        for (char c : encoded) {
            if (c == '=') break;
            auto pos = b64.find(c);
            if (pos == std::string::npos) continue;
            buf = (buf << 6) | (uint32_t)pos;
            bits += 6;
            if (bits >= 8) { bits -= 8; decoded.push_back((char)(buf >> bits)); buf &= (1u << bits) - 1; }
        }
        try {
            return nlohmann::json::parse(decoded);
        } catch (const nlohmann::json::exception&) {
            return nlohmann::json::object();
        }
    }

    uint64_t GetDiscordId() const {
        const auto claims = DecodeClaims();
        // Nakama stores custom claims in the "vrs" (vars) map
        std::string did;
        if (claims.contains("vrs") && claims["vrs"].is_object()) {
            did = claims["vrs"].value("did", "");
        }
        // Fallback: check top-level "did" for compatibility
        if (did.empty()) {
            did = claims.value("did", "");
        }
        if (!did.empty()) return strtoull(did.c_str(), nullptr, 10);
        return 0;
    }

    /// Expiry from the JWT's own `exp` claim (RFC 7519: seconds since epoch).
    /// Returns 0 when the token carries no usable `exp`.
    ///
    /// This is the authority for when to refresh. The previous code hardcoded
    /// now+60 regardless of what the server issued, so the client refreshed on
    /// its own schedule instead of the token's — and a token that was still
    /// valid for an hour was thrown away every minute.
    uint64_t GetJwtExpiry() const {
        const auto claims = DecodeClaims();
        if (claims.contains("exp") && claims["exp"].is_number_unsigned()) {
            return claims["exp"].get<uint64_t>();
        }
        return 0;
    }
};

// Fallback access-token lifetime, seconds — used ONLY when the JWT carries no
// usable `exp` claim. The token's own `exp` is the authority (see GetJwtExpiry).
//
// This was `kMaxAccessTokenLifetimeSec`, a hard 60s cap applied unconditionally,
// justified as limiting the window of a LEAKED access token. That rationale is
// about a token at rest — and the access token is never persisted: SaveToken
// writes only refresh_token + user_id + username, deliberately. So the cap was
// defending a threat this design does not have, while forcing a refresh every
// 60s no matter what the server issued.
static constexpr uint64_t kFallbackAccessTokenLifetimeSec = 300;

// Separate concern, deliberately kept (N51): an access token read FROM DISK is
// clamped hard. A legacy .credentials.json may still contain one, and a token at
// rest on disk IS the leak scenario the original cap was written for. This bound
// applies to the load path only — never to a freshly issued token, whose own
// `exp` governs.
static constexpr uint64_t kMaxDiskAccessTokenLifetimeSec = 60;

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

        // Client-side cap for a token read FROM DISK. Current code never writes
        // the access token, but a legacy file may carry one — and a token at rest
        // is exactly the leak scenario this bounds. Fresh tokens are governed by
        // their own JWT `exp` (GetJwtExpiry), not by this.
        uint64_t now = static_cast<uint64_t>(time(nullptr));
        uint64_t maxExpiry = now + kMaxDiskAccessTokenLifetimeSec;
        if (result.token_expiry > maxExpiry) {
            result.token_expiry = maxExpiry;
        }

        return result;
    } catch (...) {
        return {};
    }
}

// Saves the REFRESH TOKEN (and identity) to _local/.credentials.json.
// The access token is deliberately NOT persisted — it lives in memory only.
// Searches for existing _local/ directory with parent-directory fallback
// (same paths as LoadCachedAuthToken). Creates _local/ next to the executable
// if none found.
inline bool SaveAuthToken(const CachedAuthToken& auth) {
    if (auth.refresh_token.empty()) return false;

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
#else
        mkdir(target_dir.c_str(), 0755);
#endif
    }

    std::string path = target_dir + "/.credentials.json";

#ifndef _WIN32
    // Set restrictive umask before creating file so it's never world-readable
    mode_t old_umask = umask(0177);
#endif

    std::ofstream out(path, std::ios::trunc);
    if (!out.is_open()) {
        fprintf(stderr, "[NEVR.AUTH] Failed to open %s for writing\n", path.c_str());
#ifndef _WIN32
        umask(old_umask);
#endif
        return false;
    }

    nlohmann::json j;
    // Access token deliberately NOT written — lives in memory only (60s lifetime).
    // The refresh token is the only persistent credential.
    j["refresh_token"] = auth.refresh_token;
    j["refresh_token_expiry"] = auth.refresh_token_expiry;
    if (!auth.user_id.empty()) j["user_id"] = auth.user_id;
    if (!auth.username.empty()) j["username"] = auth.username;

    out << j.dump(2) << "\n";
    out.close();

#ifdef _WIN32
    // Hide the file and restrict to current user only
    if (!SetFileAttributesA(path.c_str(), FILE_ATTRIBUTE_HIDDEN)) {
        Log(EchoVR::LogLevel::Warning, "[NEVR.AUTH] SetFileAttributesA failed for %s: %lu",
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
                        Log(EchoVR::LogLevel::Warning, "[NEVR.AUTH] SetNamedSecurityInfoA failed for %s: %lu",
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
