/* SYNTHESIS -- custom tool code, not from binary */

#pragma once

#include "auth_token.h"
#include "nevr_curl.h"

#include <nlohmann/json.hpp>
#include <cstdio>
#include <string>
#include <vector>

// Refresh an expired access token using the refresh token.
// Calls Nakama's SessionRefresh endpoint with Basic auth (server_key).
// On success, updates auth in-place and saves to disk. Returns true on success.
inline bool RefreshAuthToken(CachedAuthToken& auth,
                             const std::string& nakama_url,
                             const std::string& server_key) {
    if (auth.refresh_token.empty()) return false;

    CURL* curl = curl_easy_init();
    if (!curl) return false;

    std::string url = nakama_url + "/v2/account/session/refresh";
    nlohmann::json body;
    body["token"] = auth.refresh_token;

    std::string post_data = body.dump();
    std::string response;

    // Nakama requires Basic auth with server key for session refresh
    // Nakama requires Basic auth with server key for session refresh.
    // Use curl's built-in Basic auth (handles base64 encoding internally).
    curl_easy_setopt(curl, CURLOPT_USERNAME, server_key.c_str());
    curl_easy_setopt(curl, CURLOPT_PASSWORD, "");
    curl_easy_setopt(curl, CURLOPT_HTTPAUTH, CURLAUTH_BASIC);

    struct curl_slist* headers = nullptr;
    headers = curl_slist_append(headers, "Content-Type: application/json");

    curl_easy_setopt(curl, CURLOPT_URL, url.c_str());
    curl_easy_setopt(curl, CURLOPT_HTTPHEADER, headers);
    curl_easy_setopt(curl, CURLOPT_POSTFIELDS, post_data.c_str());
    curl_easy_setopt(curl, CURLOPT_WRITEFUNCTION, nevr::CurlWriteCallback);
    curl_easy_setopt(curl, CURLOPT_WRITEDATA, &response);
    curl_easy_setopt(curl, CURLOPT_TIMEOUT, 10L);
    curl_easy_setopt(curl, CURLOPT_SSL_VERIFYPEER, 0L);

    CURLcode res = curl_easy_perform(curl);
    curl_slist_free_all(headers);
    curl_easy_cleanup(curl);

    if (res != CURLE_OK) {
        fprintf(stderr, "[NEVR.AUTH] Token refresh failed: %s\n", curl_easy_strerror(res));
        return false;
    }

    try {
        auto j = nlohmann::json::parse(response);

        std::string new_token = j.value("token", "");
        std::string new_refresh = j.value("refresh_token", "");

        if (new_token.empty()) {
            fprintf(stderr, "[NEVR.AUTH] Token refresh returned empty token\n");
            return false;
        }

        auth.token = new_token;
        // Default 1hr access token; TODO: parse exp from JWT claims
        auth.token_expiry = static_cast<uint64_t>(time(nullptr)) + 3600;

        if (!new_refresh.empty()) {
            auth.refresh_token = new_refresh;
            // Default 30-day refresh token; TODO: parse exp from JWT claims
            auth.refresh_token_expiry = static_cast<uint64_t>(time(nullptr)) + (30 * 24 * 3600);
        }

        SaveAuthToken(auth);
        fprintf(stderr, "[NEVR.AUTH] Token refreshed successfully\n");
        return true;
    } catch (const nlohmann::json::parse_error& e) {
        fprintf(stderr, "[NEVR.AUTH] Token refresh response parse error: %s\n", e.what());
        return false;
    }
}
