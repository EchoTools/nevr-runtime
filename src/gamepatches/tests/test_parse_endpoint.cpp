// GTest for ParseEndpoint — validates the port-parsing fix for N1.
// The production ParseEndpoint lives in gameserver/messages.cpp (two copies:
// src/gameserver/ and src/gamepatches/gameserver/). Both copies are identical.
// This test file mirrors the production algorithm independently so it can be
// compiled without the full protobuf/game-server dependency chain.
//
// N1: Uncaught std::stoul in ParseEndpoint → whole-process crash (remote DoS).
// Fix: strtoul + errno + UINT16_MAX range check, WARNING log on failure.

#include <gtest/gtest.h>

#include <cerrno>
#include <climits>
#include <cstdint>
#include <cstdlib>
#include <string>

#ifdef _WIN32
#include <ws2tcpip.h>
#else
// inet_pton stub for Linux-native test compilation (unit test only —
// the actual DLL runs under Wine where inet_pton is available).
#include <arpa/inet.h>
#endif

// Mirror of the production ParseEndpoint with the N1 fix applied.
// Identical algorithm to src/gameserver/messages.cpp:95-133
// (and src/gamepatches/gameserver/messages.cpp:95-133).
static bool ParseEndpointForTest(const std::string& endpointStr,
                                  uint32_t& internalIP, uint32_t& externalIP,
                                  uint16_t& port) {
  size_t firstColon = endpointStr.find(':');
  size_t lastColon = endpointStr.rfind(':');

  if (firstColon == std::string::npos || lastColon == std::string::npos ||
      firstColon == lastColon) {
    return false;
  }

  std::string internalStr = endpointStr.substr(0, firstColon);
  std::string externalStr =
      endpointStr.substr(firstColon + 1, lastColon - firstColon - 1);
  std::string portStr = endpointStr.substr(lastColon + 1);

  // Parse IPs using inet_pton
  struct in_addr inAddr, exAddr;
  if (inet_pton(AF_INET, internalStr.c_str(), &inAddr) != 1) return false;
  if (inet_pton(AF_INET, externalStr.c_str(), &exAddr) != 1) return false;

  internalIP = inAddr.s_addr;
  externalIP = exAddr.s_addr;

  // Parse port with errno checking (defense against N1 — std::stoul can throw)
  errno = 0;
  char* endptr = nullptr;
  unsigned long portVal = strtoul(portStr.c_str(), &endptr, 10);
  if (errno != 0 || endptr == portStr.c_str() || *endptr != '\0') {
    return false;
  }
  if (portVal > UINT16_MAX) {
    return false;
  }
  port = static_cast<uint16_t>(portVal);
  return true;
}

// ---------------------------------------------------------------------------
// (a) Valid input parses correctly
// ---------------------------------------------------------------------------

TEST(ParseEndpoint, ValidEndpoint) {
  uint32_t ip1 = 0, ip2 = 0;
  uint16_t port = 0;

  // "10.0.0.1:10.0.0.2:7777" — two valid IPs, valid port
  EXPECT_TRUE(ParseEndpointForTest("10.0.0.1:10.0.0.2:7777", ip1, ip2, port));
  EXPECT_EQ(port, 7777);
}

TEST(ParseEndpoint, ValidEndpointPortZero) {
  uint32_t ip1 = 0, ip2 = 0;
  uint16_t port = 0xFFFF;

  // port 0 is valid
  EXPECT_TRUE(ParseEndpointForTest("127.0.0.1:127.0.0.1:0", ip1, ip2, port));
  EXPECT_EQ(port, 0);
}

TEST(ParseEndpoint, ValidEndpointPortMax) {
  uint32_t ip1 = 0, ip2 = 0;
  uint16_t port = 0;

  // port 65535 (UINT16_MAX) is valid
  EXPECT_TRUE(ParseEndpointForTest("1.2.3.4:5.6.7.8:65535", ip1, ip2, port));
  EXPECT_EQ(port, 65535);
}

// ---------------------------------------------------------------------------
// (b) Malformed input is caught — does not throw or terminate
// ---------------------------------------------------------------------------

TEST(ParseEndpoint, EmptyPortString) {
  uint32_t ip1 = 0, ip2 = 0;
  uint16_t port = 0;

  // "1.2.3.4:5.6.7.8:" — trailing colon, empty port
  // This is the exact attack vector from N1.
  EXPECT_FALSE(ParseEndpointForTest("1.2.3.4:5.6.7.8:", ip1, ip2, port));
}

TEST(ParseEndpoint, NonNumericPort) {
  uint32_t ip1 = 0, ip2 = 0;
  uint16_t port = 0;

  // "1.2.3.4:5.6.7.8:abc" — non-numeric port
  EXPECT_FALSE(ParseEndpointForTest("1.2.3.4:5.6.7.8:abc", ip1, ip2, port));
}

TEST(ParseEndpoint, TrailingGarbageOnPort) {
  uint32_t ip1 = 0, ip2 = 0;
  uint16_t port = 0;

  // "1.2.3.4:5.6.7.8:7777x" — trailing non-digit, endptr != null terminator
  EXPECT_FALSE(ParseEndpointForTest("1.2.3.4:5.6.7.8:7777x", ip1, ip2, port));
}

TEST(ParseEndpoint, WhitespaceOnlyPort) {
  uint32_t ip1 = 0, ip2 = 0;
  uint16_t port = 0;

  // "1.2.3.4:5.6.7.8:   " — only whitespace, endptr == start (nothing parsed)
  EXPECT_FALSE(ParseEndpointForTest("1.2.3.4:5.6.7.8:   ", ip1, ip2, port));
}

TEST(ParseEndpoint, NegativePortNumber) {
  uint32_t ip1 = 0, ip2 = 0;
  uint16_t port = 0;

  // "-1" — strtoul accepts the minus sign, negates the result,
  // producing ULONG_MAX which exceeds UINT16_MAX. Caught by range check.
  EXPECT_FALSE(ParseEndpointForTest("1.2.3.4:5.6.7.8:-1", ip1, ip2, port));
}

// ---------------------------------------------------------------------------
// (c) Overflow input is caught
// ---------------------------------------------------------------------------

TEST(ParseEndpoint, PortOverflow) {
  uint32_t ip1 = 0, ip2 = 0;
  uint16_t port = 0;

  // "99999999999" → strtoul sets errno=ERANGE, or the value > UINT16_MAX
  EXPECT_FALSE(
      ParseEndpointForTest("1.2.3.4:5.6.7.8:99999999999", ip1, ip2, port));
}

TEST(ParseEndpoint, PortJustOverMax) {
  uint32_t ip1 = 0, ip2 = 0;
  uint16_t port = 0;

  // 65536 = UINT16_MAX + 1 — strtoul succeeds but range check fails
  EXPECT_FALSE(ParseEndpointForTest("1.2.3.4:5.6.7.8:65536", ip1, ip2, port));
}

// ---------------------------------------------------------------------------
// Edge cases: malformed endpoint structure (not the port, but the wrapper)
// ---------------------------------------------------------------------------

TEST(ParseEndpoint, MissingColons) {
  uint32_t ip1 = 0, ip2 = 0;
  uint16_t port = 0;

  // No colons at all
  EXPECT_FALSE(ParseEndpointForTest("abcdef", ip1, ip2, port));
}

TEST(ParseEndpoint, SingleColon) {
  uint32_t ip1 = 0, ip2 = 0;
  uint16_t port = 0;

  // Only one colon — can't split internal:external:port
  EXPECT_FALSE(ParseEndpointForTest("1.2.3.4:7777", ip1, ip2, port));
}

TEST(ParseEndpoint, InvalidIp) {
  uint32_t ip1 = 0, ip2 = 0;
  uint16_t port = 0;

  // Valid port but invalid IP in first position
  EXPECT_FALSE(
      ParseEndpointForTest("notanip:10.0.0.2:7777", ip1, ip2, port));
}

TEST(ParseEndpoint, EmptyString) {
  uint32_t ip1 = 0, ip2 = 0;
  uint16_t port = 0;

  EXPECT_FALSE(ParseEndpointForTest("", ip1, ip2, port));
}
