/* SYNTHESIS -- custom tool code, not from binary */

#pragma once

#include "core/auth_refresh.h"
#include "core/auth_token.h"
#include "nevr_curl.h"
#include "runtime/log/security_diagnostics.h"

#include <nlohmann/json.hpp>
#include <cstdio>
#include <string>
#include <vector>

namespace nevr::auth {
// Windows-side adapter: the portable core logs with its own level enum.
inline EchoVR::LogLevel ToEchoLogLevel(LogLevel level) {
    switch (level) {
        case LogLevel::Debug: return EchoVR::LogLevel::Debug;
        case LogLevel::Info: return EchoVR::LogLevel::Info;
        case LogLevel::Warning: return EchoVR::LogLevel::Warning;
        case LogLevel::Error: return EchoVR::LogLevel::Error;
    }
    return EchoVR::LogLevel::Warning;
}
}  // namespace nevr::auth

// Refresh an expired access token using the refresh token.
// Calls the custom device/auth/refresh RPC (not the standard Nakama session
// refresh, which uses a different signing key and requires session cache).
// On success, updates auth in-place and saves to disk. Returns true on success.
//
// The request body, the response interpretation and the "failure leaves `auth`
// untouched" contract are the platform-neutral nevr::auth core (core/auth_refresh.h),
// shared with the Quest shim; only the libcurl transport and the cache write are
// Windows-side.
inline bool RefreshAuthToken(CachedAuthToken& auth,
                             const std::string& nakama_url,
                             const std::string& http_key) {
    if (auth.refresh_token.empty()) return false;

    CURL* curl = curl_easy_init();
    if (!curl) return false;

    const std::string url = nevr::auth::BuildDeviceAuthUrl(nakama_url, http_key, "refresh");
    const std::string post_data = nevr::auth::BuildRefreshBody(auth.refresh_token);
    std::string response;

    // No Basic auth needed -- the RPC uses http_key in the query param

    struct curl_slist* headers = nullptr;
    headers = curl_slist_append(headers, "Content-Type: application/json");

    curl_easy_setopt(curl, CURLOPT_URL, url.c_str());
    curl_easy_setopt(curl, CURLOPT_HTTPHEADER, headers);
    curl_easy_setopt(curl, CURLOPT_POSTFIELDS, post_data.c_str());
    curl_easy_setopt(curl, CURLOPT_WRITEFUNCTION, nevr::CurlWriteCallback);
    curl_easy_setopt(curl, CURLOPT_WRITEDATA, &response);
    curl_easy_setopt(curl, CURLOPT_TIMEOUT, 10L);
#ifdef NEVR_INSECURE_SKIP_TLS_VERIFY
    curl_easy_setopt(curl, CURLOPT_SSL_VERIFYPEER, 0L);
    curl_easy_setopt(curl, CURLOPT_SSL_VERIFYHOST, 0L);
#else
    curl_easy_setopt(curl, CURLOPT_SSL_VERIFYPEER, 1L);
    curl_easy_setopt(curl, CURLOPT_SSL_VERIFYHOST, 2L);
#endif

    const CURLcode res = curl_easy_perform(curl);

    long http_code = 0;
    curl_easy_getinfo(curl, CURLINFO_RESPONSE_CODE, &http_code);

    curl_slist_free_all(headers);
    curl_easy_cleanup(curl);

    nevr::auth::HttpResponse http;
    http.transport_ok = (res == CURLE_OK);
    http.transport_code = static_cast<int>(res);
    http.status = http_code;
    http.body = std::move(response);

    const auto sink = [](nevr::auth::LogLevel level, const std::string& message) {
        Log(nevr::auth::ToEchoLogLevel(level), "%s", message.c_str());
    };
    if (nevr::auth::ApplyRefreshResponse(auth, http, static_cast<uint64_t>(time(nullptr)), sink) !=
        nevr::auth::RefreshOutcome::Refreshed) {
        return false;
    }

    SaveAuthToken(auth);
    // No success log here: the callers (token_auth.cpp, gameserver.cpp) log the
    // success at Info with their own detail, so a line here would duplicate it.
    return true;
}
