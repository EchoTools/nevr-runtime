#include "upnp.h"

#define WIN32_LEAN_AND_MEAN
#include <windows.h>

// miniupnpc headers (included after windows.h to avoid conflicts)
#include <miniupnpc/miniupnpc.h>
#include <miniupnpc/upnpcommands.h>
#include <miniupnpc/upnperrors.h>

#include <cstdio>
#include <cstring>

#include "echovr.h"

// Forward-declare Log from gameserver.cpp (same DLL, no extra header needed)
extern void Log(EchoVR::LogLevel level, const char* format, ...);

bool UPnPHelper::s_active = false;
uint16_t UPnPHelper::s_mappedExternalPort = 0;

bool UPnPHelper::OpenPort(uint16_t internalPort, uint16_t externalPort,
                          std::string& outExternalIp) {
  if (externalPort == 0) externalPort = internalPort;

  int error = 0;
  UPNPDev* devlist = upnpDiscover(2000, nullptr, nullptr,
                                  UPNP_LOCAL_PORT_ANY, 0, 2, &error);
  if (!devlist) {
    Log(EchoVR::LogLevel::Warning,
        "[NEVR.UPNP] No UPnP devices found (error=%d)", error);
    return false;
  }

  UPNPUrls urls  = {};
  IGDdatas  data = {};
  char      lanIp[64] = {};
  char      wanIp[64] = {};

  // UPNP_GetValidIGD signature (miniupnpc 2.x):
  // int UPNP_GetValidIGD(UPNPDev*, UPNPUrls*, IGDdatas*,
  //                      char* lanaddr, int lanaddrlen,
  //                      char* wanaddr, int wanaddrlen)
  int igd = UPNP_GetValidIGD(devlist, &urls, &data,
                              lanIp,  sizeof(lanIp),
                              wanIp,  sizeof(wanIp));
  freeUPNPDevlist(devlist);

  if (igd != 1 && igd != 2) {
    Log(EchoVR::LogLevel::Warning,
        "[NEVR.UPNP] No valid IGD found (result=%d)", igd);
    FreeUPNPUrls(&urls);
    return false;
  }

  // Only populate outExternalIp if the caller hasn't already provided an override.
  // Preserves config.json "external_ip" when UPnP is also enabled.
  if (outExternalIp.empty()) {
    if (wanIp[0] != '\0') {
      outExternalIp = wanIp;
    } else {
      char externalIpBuf[64] = {};
      if (UPNP_GetExternalIPAddress(urls.controlURL,
                                    data.first.servicetype,
                                    externalIpBuf) == UPNPCOMMAND_SUCCESS) {
        outExternalIp = externalIpBuf;
      }
    }
  }

  char internalPortStr[8], externalPortStr[8];
  snprintf(internalPortStr, sizeof(internalPortStr), "%u", internalPort);
  snprintf(externalPortStr, sizeof(externalPortStr), "%u", externalPort);

  int r = UPNP_AddPortMapping(
      urls.controlURL, data.first.servicetype,
      externalPortStr, internalPortStr, lanIp,
      "EchoVR Game Server", "UDP",
      nullptr, "0");  // remoteHost=any, leaseDuration=0 (permanent)

  FreeUPNPUrls(&urls);

  if (r != UPNPCOMMAND_SUCCESS) {
    Log(EchoVR::LogLevel::Warning,
        "[NEVR.UPNP] AddPortMapping failed: %s (code=%d)",
        strupnperror(r), r);
    return false;
  }

  s_active = true;
  s_mappedExternalPort = externalPort;
  Log(EchoVR::LogLevel::Info,
      "[NEVR.UPNP] Port mapping added: %u (ext) -> %u (int) UDP, WAN IP: %s",
      externalPort, internalPort, outExternalIp.c_str());
  return true;
}

void UPnPHelper::ClosePort() {
  if (!s_active) return;

  int error = 0;
  UPNPDev* devlist = upnpDiscover(2000, nullptr, nullptr,
                                  UPNP_LOCAL_PORT_ANY, 0, 2, &error);
  if (!devlist) {
    Log(EchoVR::LogLevel::Warning,
        "[NEVR.UPNP] ClosePort: no UPnP devices found — mapping may persist");
    s_active = false;
    return;
  }

  UPNPUrls urls  = {};
  IGDdatas  data = {};
  char      lanIp[64] = {};
  char      wanIp[64] = {};

  int igd = UPNP_GetValidIGD(devlist, &urls, &data,
                              lanIp, sizeof(lanIp),
                              wanIp, sizeof(wanIp));
  freeUPNPDevlist(devlist);

  if (igd == 1 || igd == 2) {
    char portStr[8];
    snprintf(portStr, sizeof(portStr), "%u", s_mappedExternalPort);
    int r = UPNP_DeletePortMapping(urls.controlURL,
                                   data.first.servicetype,
                                   portStr, "UDP", nullptr);
    if (r != UPNPCOMMAND_SUCCESS) {
      Log(EchoVR::LogLevel::Warning,
          "[NEVR.UPNP] DeletePortMapping failed: %s (code=%d)",
          strupnperror(r), r);
    } else {
      Log(EchoVR::LogLevel::Info,
          "[NEVR.UPNP] Port mapping removed: %u UDP",
          s_mappedExternalPort);
    }
    FreeUPNPUrls(&urls);
  }

  s_active = false;
  s_mappedExternalPort = 0;
}
