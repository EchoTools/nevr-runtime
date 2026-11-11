#pragma once

// The address the game's broadcaster reports for itself (`Broadcaster::data->addr`) is a 128-byte
// sockaddr_storage-sized slot. Registration reads it as IPv4 only; a slot of another family used to
// be read as a `sockaddr_in` anyway and produced a meaningless address. This header reads the family
// first and names what is there when it is not IPv4 (#568, unit A4).

#include <winsock2.h>
#include <ws2tcpip.h>

#include <cstddef>
#include <cstdint>
#include <cstdio>
#include <cstring>
#include <string>

#include "abi/echovr.h"
#include "core/logging.h"

namespace nevr_game_server {

struct BroadcasterAddress {
  uint16_t family = 0;  // sa_family_t of the slot (2 = AF_INET, 23 = AF_INET6 on Windows)
  bool isIpv4 = false;  // family AF_INET, or 0: an unset slot, read as IPv4 as the registration always has
  // Dotted quad when isIpv4. Otherwise the slot's own address as text: RFC 5952 form for AF_INET6,
  // the 16 bytes after the family, in hex, for any other family. Empty when those bytes are all zero.
  std::string text;
};

inline BroadcasterAddress ReadBroadcasterAddress(const EchoVR::AddressInfo& slot) {
  BroadcasterAddress out;
  uint8_t bytes[sizeof(slot.raw)];
  std::memcpy(bytes, slot.raw, sizeof(bytes));
  std::memcpy(&out.family, bytes, sizeof(out.family));

  if (out.family == AF_INET || out.family == 0) {
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

// The broadcaster's own address as the registration's default internal IP. A slot of a nonzero
// family other than AF_INET is named in a Warning with its family and address, and yields an empty
// string, so the config or UPnP override decides the address (the registration then carries none if
// neither is set). Family 0, an unset slot, reads as IPv4 and logs nothing, as before.
inline std::string BroadcasterInternalIp(const EchoVR::AddressInfo& slot, const char* site) {
  const BroadcasterAddress address = ReadBroadcasterAddress(slot);
  if (address.isIpv4) return address.text;
  Log(EchoVR::LogLevel::Warning,
      "[NEVR.GAMESERVER] broadcaster address is not IPv4 site=%s family=%u address=%s"
      " — registration uses the config or UPnP address, none if unset",
      site, static_cast<unsigned>(address.family), address.text.empty() ? "none" : address.text.c_str());
  return std::string();
}

}  // namespace nevr_game_server
