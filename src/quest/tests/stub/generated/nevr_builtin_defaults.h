// Host-test stand-in for the configure-time embedded defaults. The two secrets are synthetic
// markers that the test asserts never reach a log line.
#pragma once

namespace nevr_builtin {

inline constexpr const char* kSocketUri = "wss://stub-emb.example/nevr";
inline constexpr const char* kHttpUri = "https://stub-emb.example:7350";
inline constexpr const char* kPublicApiKey = "STUB-EMBEDDED-API-SECRET-5521";
inline constexpr const char* kPublicSocketKey = "STUB-EMBEDDED-SERVER-SECRET-9973";

}  // namespace nevr_builtin
