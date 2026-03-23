#pragma once
#include <cstdint>
#include <string>

/// UPnP port forwarding helper for the broadcaster UDP port.
/// Static class — manages a single active port mapping.
/// Not thread-safe; call only from the server initialization thread.
class UPnPHelper {
 public:
  /// Discover the IGD and map internalPort -> externalPort (UDP).
  /// If externalPort is 0, uses internalPort for both.
  /// On success, populates outExternalIp with the router's WAN IP.
  /// If outExternalIp is already non-empty (caller-provided override), preserves it.
  /// Logs a warning and returns false on failure — callers should continue.
  static bool OpenPort(uint16_t internalPort, uint16_t externalPort,
                       std::string& outExternalIp);

  /// Remove the active port mapping. No-op if none is active.
  static void ClosePort();

 private:
  static bool     s_active;
  static uint16_t s_mappedExternalPort;
};
