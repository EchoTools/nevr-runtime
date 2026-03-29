#pragma once

#include <cstdint>
#include <string>

// Device code authentication with local credential caching.
// Implements the echovrce.com device code flow (Discord OAuth on web)
// and persists tokens to _local/auth.json for reuse across launches.

class DeviceAuth {
public:
    DeviceAuth() = default;
    ~DeviceAuth() = default;

    // Configure API endpoint and HTTP key from _local/config.json values.
    void Configure(const std::string& url, const std::string& httpKey);

    // Try loading a cached token from _local/auth.json.
    // Returns true if a valid (non-expired) token was found.
    bool TryLoadCachedToken();

    // Run the full device code flow: request code, display in console,
    // open browser, poll for verification, save token on success.
    bool RunDeviceAuthFlow();

    // Save current token to _local/auth.json.
    bool SaveToken();

    // Check if we have a valid (non-expired) session.
    bool IsAuthenticated() const;

private:
    std::string RequestDeviceCode();
    std::string PollDeviceCode(const std::string& code);
    std::string HttpPostPublic(const std::string& url, const std::string& body);
    void DisplayLinkingCode(const std::string& code);

    std::string m_url;
    std::string m_httpKey;
    std::string m_token;
    uint64_t m_tokenExpiry = 0;
    bool m_configured = false;
};
