#pragma once
// The lobby session records (logging.md Rule 16) for one scenario, pinned byte for byte (#1429).
//
// ws_bridge.cpp (PC) and session_router.cpp (Quest) both write these lines from the same frames. Both test suites,
// test_behavioral.cpp and session_router_test.cpp, include this header, build the wire bytes here and compare what
// their writer emits with the expected text written here. A writer that differs from the other fails its own test
// against these same bytes, so the two cannot disagree silently.
//
// The scenario, in clock milliseconds: the game's matchmaker (connection 2) sends a find at 1000; the service answers
// with a success at 1250 and a failure at 1300. The party is id 7 with three members, led by account 42; the local
// account is 5, so the local account does not lead it.

#include <cstddef>
#include <cstdint>
#include <string>

#include "runtime/compat/lobby_frame.h"

namespace lobby_record_golden {

inline constexpr uint64_t kMode = 0x1234567890abcdefULL;  // the mode field of a success or failure (not logged)
inline constexpr int kConn = 2;
inline constexpr uint64_t kFindAt = 1000;
inline constexpr uint64_t kSuccessAt = 1250;  // 250 ms after the find
inline constexpr uint64_t kFailureAt = 1300;   // 300 ms after the find, and the second answer to it
inline constexpr uint64_t kPartyId = 7;
inline constexpr uint64_t kPartyMembers = 3;
inline constexpr uint64_t kPartyOwner = 42;
inline constexpr uint64_t kPartySelf = 5;
inline constexpr char kSentinelMessage[] = "sentinel-remote-message";  // 23 bytes: the failure's message_bytes

inline void PutLe(std::string& out, uint64_t value, std::size_t bytes) {
  for (std::size_t i = 0; i < bytes; ++i) out.push_back(static_cast<char>((value >> (8 * i)) & 0xff));
}

// A GUID as the wire stores it (Windows layout): the text 44332211-6655-8877-99AA-BBCCDDEEFF<tail>, the tail as a byte.
inline std::string Guid(uint8_t tail) {
  const uint8_t bytes[16] = {0x11, 0x22, 0x33, 0x44, 0x55, 0x66, 0x77, 0x88,
                             0x99, 0xaa, 0xbb, 0xcc, 0xdd, 0xee, 0xff, tail};
  return std::string(reinterpret_cast<const char*>(bytes), sizeof(bytes));
}

// A success payload: mode, lobby Guid(0xc0), group Guid(0xb0), then the endpoint, flags and keys (not read).
inline std::string SuccessPayload() {
  std::string out;
  PutLe(out, kMode, 8);
  out += Guid(0xc0);
  out += Guid(0xb0);
  out.append(64, '\0');
  return out;
}

// A failure payload: mode, group Guid(0xb0), code, pad, the message field (64 bytes, cut and NUL-filled), expiry.
inline std::string FailureWithMessage(uint32_t code, const std::string& message) {
  std::string out;
  PutLe(out, kMode, 8);
  out += Guid(0xb0);
  PutLe(out, code, 4);
  PutLe(out, 0, 4);
  std::string field = message.substr(0, 64);
  field.resize(64, '\0');
  out += field;
  out.append(8, '\0');
  return out;
}

inline std::string FailurePayload(uint32_t code) { return FailureWithMessage(code, kSentinelMessage); }

// The record of the success that answers the scenario's find (250 ms after it, the first answer to it).
inline constexpr char kSuccessAfterFind[] =
    "[NEVR.WS] lobby session success conn=2 lobby=44332211-6655-8877-99AA-BBCCDDEEFFC0 "
    "group=44332211-6655-8877-99AA-BBCCDDEEFFB0 party_id=7 party_size=3 party_leader=no "
    "last_request=find last_request_age_ms=250 request_outstanding=yes";

// The record of the failure after that success (300 ms after the find, a second answer, so not outstanding).
inline constexpr char kFailureAfterSuccess[] =
    "[NEVR.WS] lobby session failure conn=2 code=8 reason=lobby_full "
    "group=44332211-6655-8877-99AA-BBCCDDEEFFB0 message_bytes=23 party_id=7 party_size=3 party_leader=no "
    "last_request=find last_request_age_ms=300 request_outstanding=no";

// A success with no request sent and no party: the request fields read none, the party reads none.
inline constexpr char kSuccessNoRequestNoParty[] =
    "[NEVR.WS] lobby session success conn=2 lobby=44332211-6655-8877-99AA-BBCCDDEEFFC0 "
    "group=44332211-6655-8877-99AA-BBCCDDEEFFB0 party_id=none party_size=0 party_leader=no "
    "last_request=none last_request_age_ms=none request_outstanding=no";

}  // namespace lobby_record_golden
