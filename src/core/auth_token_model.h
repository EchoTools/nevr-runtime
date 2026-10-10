/* SYNTHESIS -- custom tool code, not from binary */

#pragma once

// Platform-neutral token model: the cached-credential record, JWT claim
// decoding, and the expiry rules. No Windows headers, no logging, no file or
// network I/O, so the Windows runtime and the Quest (Android/NDK) shim compile
// the same source. The Windows cache-file location and ACL handling is in
// core/auth_token.h.

#include <nlohmann/json.hpp>

#include <cstdint>
#include <ctime>
#include <optional>
#include <string>
#include <string_view>

struct CachedAuthToken {
    std::string token;
    uint64_t token_expiry = 0;
    std::string refresh_token;
    uint64_t refresh_token_expiry = 0;
    std::string user_id;
    std::string username;

    // `now` is injectable so the portable auth core (and its host tests) can
    // drive expiry with a fake clock; the no-argument forms read the wall clock.
    bool HasValidToken(uint64_t now) const {
        return !token.empty() && token_expiry > now + 60;
    }
    bool HasValidToken() const { return HasValidToken(static_cast<uint64_t>(time(nullptr))); }

    bool HasValidRefreshToken(uint64_t now) const {
        return !refresh_token.empty() && refresh_token_expiry > now + 60;
    }
    bool HasValidRefreshToken() const { return HasValidRefreshToken(static_cast<uint64_t>(time(nullptr))); }

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
            buf = (buf << 6) | static_cast<uint32_t>(pos);
            bits += 6;
            if (bits >= 8) { bits -= 8; decoded.push_back(static_cast<char>(buf >> bits)); buf &= (1u << bits) - 1; }
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

// Fallback refresh-token lifetime, seconds — used ONLY when the server does not
// state one. This 30-day number is a client GUESS at a SERVER policy: nothing on
// this side measured it, and if the server shortens its refresh lifetime a client
// holding this constant keeps presenting a credential that died days ago, and
// skips the device flow it should have run.
//
// RFC 6749 §5.1 `refresh_token_expires_in` is the authority and the server now
// sends it (EchoTools/nakama f945f631d, device/auth/poll + device/auth/refresh).
// This exists solely for a nakama older than that commit, which omits the field.
// It is a fallback, not a fact. Delete it once no such server is deployed.
static constexpr uint64_t kFallbackRefreshTokenLifetimeSec = 30 * 24 * 3600;

/// Reads an RFC 6749 seconds-from-now field, or nullopt when the server did not
/// state one. Absence stays absent — the caller must reach for a documented
/// fallback rather than receive a number this side made up.
///
/// is_number_unsigned() is the guard, not is_number(): the server computes both
/// expiry fields as time.Until(deadline).Seconds() and they go NEGATIVE once the
/// deadline has passed (EchoTools/nakama f945f631d). nlohmann parses a negative
/// literal as signed, so this rejects it — adding it to `now` would move the
/// expiry backwards into the past.
inline std::optional<uint64_t> ReadExpiresInSeconds(const nlohmann::json& j, const char* field) {
    if (!j.contains(field)) return std::nullopt;
    const auto& value = j.at(field);
    if (!value.is_number_unsigned()) return std::nullopt;
    return value.get<uint64_t>();
}

/// Absolute unix expiry for a FRESHLY ISSUED access token, in priority order:
///   1. the JWT's own `exp` claim (RFC 7519) — the issuer's own statement;
///   2. RFC 6749 §5.1 `expires_in`, seconds from now, as sent by the server;
///   3. kFallbackAccessTokenLifetimeSec, only when neither is available.
/// Never applies kMaxDiskAccessTokenLifetimeSec — that bound is for a token read
/// from disk, which is a different threat (see the constant's comment).
inline uint64_t ResolveAccessTokenExpirySec(uint64_t now, const std::string& access_token,
                                            std::optional<uint64_t> expires_in) {
    CachedAuthToken probe;
    probe.token = access_token;
    const uint64_t jwtExpiry = probe.GetJwtExpiry();
    if (jwtExpiry > now) return jwtExpiry;
    if (expires_in.has_value()) return now + *expires_in;
    return now + kFallbackAccessTokenLifetimeSec;
}

/// Absolute unix expiry for a refresh token. The server's
/// `refresh_token_expires_in` when it sent one; otherwise the fallback constant,
/// which is a guess and is documented as one.
inline uint64_t ResolveRefreshTokenExpirySec(uint64_t now,
                                             std::optional<uint64_t> refresh_token_expires_in) {
    if (refresh_token_expires_in.has_value()) return now + *refresh_token_expires_in;
    return now + kFallbackRefreshTokenLifetimeSec;
}


/// Parses the text of a .credentials.json into a token record. Malformed text
/// yields an empty record (never throws). `now` is the clock used for the
/// at-rest clamp on a legacy access token (kMaxDiskAccessTokenLifetimeSec).
inline CachedAuthToken ParseCredentialsJson(std::string_view text, uint64_t now) {
    try {
        const auto json = nlohmann::json::parse(text);
        CachedAuthToken result;
        result.token = json.value("token", "");
        result.token_expiry = json.value("token_expiry", uint64_t(0));
        // Backwards compat: old format used "expiry" for token expiry.
        if (result.token_expiry == 0) result.token_expiry = json.value("expiry", uint64_t(0));
        result.refresh_token = json.value("refresh_token", "");
        result.refresh_token_expiry = json.value("refresh_token_expiry", uint64_t(0));
        result.user_id = json.value("user_id", "");
        result.username = json.value("username", "");

        // Bound legacy access tokens at rest; fresh tokens use their JWT exp.
        const uint64_t maxExpiry = now + kMaxDiskAccessTokenLifetimeSec;
        if (result.token_expiry > maxExpiry) result.token_expiry = maxExpiry;
        return result;
    } catch (const nlohmann::json::exception&) {
        return {};
    }
}

/// The text of a .credentials.json for `auth`. The access token is deliberately
/// NOT written -- it lives in memory only; the refresh token is the one
/// persistent credential. (A live access token lasts as long as its own JWT
/// `exp` says -- one hour from nakama -- and that number exists nowhere on
/// disk, so no reader of this file can answer when the access token expires.
/// The 60s cap, kMaxDiskAccessTokenLifetimeSec, applies to the load path only;
/// it is not the access token's lifetime.)
inline std::string SerializeCredentialsJson(const CachedAuthToken& auth) {
    nlohmann::json j;
    j["refresh_token"] = auth.refresh_token;
    j["refresh_token_expiry"] = auth.refresh_token_expiry;
    if (!auth.user_id.empty()) j["user_id"] = auth.user_id;
    if (!auth.username.empty()) j["username"] = auth.username;
    return j.dump(2) + "\n";
}
