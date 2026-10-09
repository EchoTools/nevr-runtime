#include "runtime/server/upnp.h"

#define WIN32_LEAN_AND_MEAN
#include <windows.h>

// miniupnpc headers (included after windows.h to avoid conflicts)
#include <miniupnpc/miniupnpc.h>
#include <miniupnpc/upnpcommands.h>
#include <miniupnpc/upnperrors.h>

#include <cstdio>
#include <cstring>

#include "abi/echovr.h"

// Forward-declare Log from gameserver.cpp (same DLL, no extra header needed)
extern void Log(EchoVR::LogLevel level, const char* format, ...);

namespace {

// What one discovery pass found. `igd` is UPNP_GetValidIGD's result (1 or 2 = a valid IGD); `urls`
// is only meaningful then and the caller frees it. `devicesFound` false means no UPnP device answered.
struct IgdDiscovery {
  bool devicesFound = false;
  int error = 0;
  int igd = 0;
  UPNPUrls urls = {};
  IGDdatas data = {};
  char lanIp[64] = {};
  char wanIp[64] = {};
};

IgdDiscovery DiscoverIgd() {
  IgdDiscovery found;
  UPNPDev* devlist = upnpDiscover(2000, nullptr, nullptr, UPNP_LOCAL_PORT_ANY, 0, 2, &found.error);
  if (!devlist) return found;
  found.devicesFound = true;
  // UPNP_GetValidIGD signature (miniupnpc 2.x):
  // int UPNP_GetValidIGD(UPNPDev*, UPNPUrls*, IGDdatas*,
  //                      char* lanaddr, int lanaddrlen,
  //                      char* wanaddr, int wanaddrlen)
  found.igd = UPNP_GetValidIGD(devlist, &found.urls, &found.data, found.lanIp, sizeof(found.lanIp),
                               found.wanIp, sizeof(found.wanIp));
  freeUPNPDevlist(devlist);
  return found;
}

}  // namespace

bool UPnPHelper::s_active = false;
uint16_t UPnPHelper::s_mappedExternalPort = 0;

bool UPnPHelper::OpenPort(uint16_t internalPort, uint16_t externalPort,
                          std::string& outExternalIp) {
  if (externalPort == 0) externalPort = internalPort;

  IgdDiscovery found = DiscoverIgd();
  if (!found.devicesFound) {
    Log(EchoVR::LogLevel::Warning,
        "[NEVR.UPNP] No UPnP devices found (error=%d) — continues without automatic port mapping;"
        " forward the port manually if NAT requires it", found.error);
    return false;
  }
  UPNPUrls& urls = found.urls;
  IGDdatas& data = found.data;
  const char* lanIp = found.lanIp;
  const char* wanIp = found.wanIp;
  const int igd = found.igd;

  if (igd != 1 && igd != 2) {
    Log(EchoVR::LogLevel::Warning,
        "[NEVR.UPNP] No valid IGD found (result=%d) — continues without automatic port mapping;"
        " forward the port manually if NAT requires it", igd);
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
        "[NEVR.UPNP] AddPortMapping failed: %s (code=%d) ext_port=%u int_port=%u",
        strupnperror(r), r, externalPort, internalPort);
    return false;
  }

  s_active = true;
  s_mappedExternalPort = externalPort;
  // N122. This was Debug — off in production — while EVERY failure path here logs
  // at Warning. So a working UPnP mapping and a UPnP that never ran produced
  // byte-identical output: nothing. Four captured runs were read as "UPnP never
  // fired" when it had in fact discovered an IGD and added the mapping every time.
  // Whether this server's port is actually forwarded is exactly what an operator
  // needs during an incident, which is the standard's definition of Info.
  Log(EchoVR::LogLevel::Info,
      "[NEVR.UPNP] Port mapping added: %u (ext) -> %u (int) UDP, WAN IP: %s",
      externalPort, internalPort, outExternalIp.c_str());
  return true;
}

void UPnPHelper::ClosePort() {
  if (!s_active) return;

  IgdDiscovery found = DiscoverIgd();
  if (!found.devicesFound) {
    Log(EchoVR::LogLevel::Warning,
        "[NEVR.UPNP] ClosePort: no UPnP devices found — mapping may persist");
    s_active = false;
    return;
  }
  UPNPUrls& urls = found.urls;
  IGDdatas& data = found.data;
  const int igd = found.igd;

  if (igd == 1 || igd == 2) {
    char portStr[8];
    snprintf(portStr, sizeof(portStr), "%u", s_mappedExternalPort);
    int r = UPNP_DeletePortMapping(urls.controlURL,
                                   data.first.servicetype,
                                   portStr, "UDP", nullptr);
    if (r != UPNPCOMMAND_SUCCESS) {
      Log(EchoVR::LogLevel::Warning,
          "[NEVR.UPNP] DeletePortMapping failed: %s (code=%d) port=%u",
          strupnperror(r), r, s_mappedExternalPort);
    } else {
      Log(EchoVR::LogLevel::Debug,
          "[NEVR.UPNP] Port mapping removed: %u UDP",
          s_mappedExternalPort);
    }
    FreeUPNPUrls(&urls);
  }

  s_active = false;
  s_mappedExternalPort = 0;
}
