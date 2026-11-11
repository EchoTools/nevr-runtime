#pragma once

// The address the game's broadcaster reports for itself (`Broadcaster::data->addr`) is a 128-byte
// sockaddr_storage-sized slot. Registration reads it as IPv4 only; a slot of another family used to
// be read as a `sockaddr_in` anyway and produced a meaningless address. This header reads the family
// first and names what is there when it is not IPv4 (#568, unit A4).

#include <winsock2.h>
#include <ws2tcpip.h>

#include <cstdint>
#include <cstdio>
#include <cstring>
#include <string>

#include "abi/echovr.h"

namespace nevr_game_server {

struct BroadcasterAddress {
  uint16_t family = 0;  // sa_family_t of the slot (2 = AF_INET, 23 = AF_INET6 on Windows)
  bool isIpv4 = false;
  // Dotted quad when isIpv4. Otherwise the slot's own address as text: RFC 5952 form for AF_INET6,
  // 16 hex bytes from sa_data for any other family. Empty when the slot is all zero.
  std::string text;
};

inline BroadcasterAddress ReadBroadcasterAddress(const EchoVR::AddressInfo& slot) {
  BroadcasterAddress out;
  uint8_t bytes[sizeof(slot.raw)];
  std::memcpy(bytes, slot.raw, sizeof(bytes));
  std::memcpy(&out.family, bytes, sizeof(out.family));

  if (out.family == AF_INET) {
    uint32_t ip = 0;
    std::memcpy(&ip, bytes + offsetof(sockaddr_in, sin_addr), sizeof(ip));
    char buf[16];
    std::snprintf(buf, sizeof(buf), "%u.%u.%u.%u", (ip >> 0) & 0xFF, (ip >> 8) & 0xFF, (ip >> 16) & 0xFF,
                  (ip >> 24) & 0xFF);
    out.isIpv4 = true;
    out.text = buf;
    return out;
  }

  if (out.family == AF_INET6) {
    char buf[INET6_ADDRSTRLEN] = {};
    if (inet_ntop(AF_INET6, bytes + offsetof(sockaddr_in6, sin6_addr), buf, sizeof(buf)) != nullptr) out.text = buf;
    return out;
  }

  char hex[2 * 16 + 1];
  bool any = false;
  for (int i = 0; i < 16; ++i) {
    std::snprintf(hex + 2 * i, 3, "%02x", bytes[2 + i]);
    any = any || bytes[2 + i] != 0;
  }
  if (any) out.text = hex;
  return out;
}

}  // namespace nevr_game_server
