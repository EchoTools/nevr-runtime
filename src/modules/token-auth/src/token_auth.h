// token_auth.h
#pragma once

#include <cstdint>
#include <string>

namespace TokenAuth {
void Init(uintptr_t base_addr, bool is_server);
void Shutdown();

// Returns the current valid Bearer token, or empty string if not authenticated.
// Thread-safe — called from WS bridge connection handlers.
std::string GetToken();

// Returns the discord ID from the current JWT, or 0 if not authenticated.
uint64_t GetDiscordId();
}
