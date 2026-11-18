#pragma once

// The text form of a GUID the runtime sends to ServerDB: lowercase hex in 8-4-4-4-12 groups.

#include <windows.h>

#include <cstdio>
#include <string>

// Helper to convert GUID to UUID string format
inline std::string GuidToUuidString(const GUID& guid) {
  char buf[64];
  snprintf(buf, sizeof(buf), "%08lx-%04x-%04x-%02x%02x-%02x%02x%02x%02x%02x%02x",
           static_cast<unsigned long>(guid.Data1), guid.Data2, guid.Data3, guid.Data4[0], guid.Data4[1], guid.Data4[2],
           guid.Data4[3], guid.Data4[4], guid.Data4[5], guid.Data4[6], guid.Data4[7]);
  return buf;
}
