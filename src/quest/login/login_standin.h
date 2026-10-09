#pragma once
// The values the login prerequisites (login_prerequisites.h) hand the game in place of an Oculus
// answer it did not get, and the predicates that recognise them again.
//
// They are generated once per process from the system's random source, so no two headsets share
// a value: a stand-in is never a constant that could link devices if one ever escaped. They are
// unambiguous inside the process by exact comparison with the generated value. The login send gate
// (FinishLogin, login_rewrite.h) blocks any login whose wire account id, access_token or nonce is a
// stand-in, so within the gates a stand-in never leaves the process on the login; what can still
// read one is listed in login_prerequisites.h ("residual").
//
// Everything here is allocation-free, lock-free and noexcept: the predicates run inside the game's
// OVR callbacks and the login send hook (built -fno-exceptions).

#include <cstddef>
#include <cstdint>

namespace QuestLogin::StandIn {

// Fills the stand-ins from arc4random_buf. Idempotent and safe to call from several threads; the
// first call generates, later calls return at once. Until it has run every accessor returns 0 or ""
// and every predicate is false.
void Generate() noexcept;

std::uint64_t OrgId() noexcept;       // neither 0 nor -1
const char* AccessToken() noexcept;   // 40 lowercase hex characters
const char* Nonce() noexcept;         // 32 lowercase hex characters
const char* OculusId() noexcept;      // "player-" + 8 hex: shorter than the game's 36-byte buffer

bool IsOrgId(std::uint64_t value) noexcept;
bool IsAccessToken(const char* value) noexcept;
bool IsNonce(const char* value) noexcept;
// Compares at most `capacity` bytes of a NUL-terminated or full fixed buffer (the game's 0x70e470).
bool IsOculusId(const char* value, std::size_t capacity = 64) noexcept;

// Test support: fixes the stand-ins to known values (any later Generate keeps them).
void SetForTest(std::uint64_t org, const char* token, const char* nonce, const char* oculus_id) noexcept;
// Test support: back to the unpublished state, so the next Generate draws fresh values.
void ResetForTest() noexcept;

}  // namespace QuestLogin::StandIn
