#include "xpid_patch.h"

#include "gamepatches_internal.h"
#include "common/logging.h"
#include "patch_addresses.h"

// Expected original bytes at each patch site (for validation).
static const BYTE kPsnShort[] = {0x50, 0x53, 0x4E, 0x00};  // "PSN\0"
static const BYTE kPsnDash[]  = {0x50, 0x53, 0x4E, 0x2D};  // "PSN-"

// Replacement bytes.
static const BYTE kDscShort[] = {0x44, 0x53, 0x43, 0x00};  // "DSC\0"
static const BYTE kDscDash[]  = {0x44, 0x53, 0x43, 0x2D};  // "DSC-"

static bool ValidateBytes(const CHAR* base, uintptr_t offset, const BYTE* expected, size_t len) {
  const BYTE* site = reinterpret_cast<const BYTE*>(base + offset);
  return memcmp(site, expected, len) == 0;
}

VOID PatchDscProvider() {
  using namespace PatchAddresses;
  const CHAR* base = EchoVR::g_GameBaseAddress;

  // Validate all three sites before patching any.
  bool ok = true;
  if (!ValidateBytes(base, XPID_PLATFORM_SHORT_NAME, kPsnShort, sizeof(kPsnShort))) {
    Log(EchoVR::LogLevel::Error,
        "[NEVR.XPID] Short name mismatch at RVA 0x%X — expected \"PSN\\0\"", XPID_PLATFORM_SHORT_NAME);
    ok = false;
  }
  if (!ValidateBytes(base, XPID_PLATFORM_DASH_PREFIX, kPsnDash, sizeof(kPsnDash))) {
    Log(EchoVR::LogLevel::Error,
        "[NEVR.XPID] Dash prefix mismatch at RVA 0x%X — expected \"PSN-\"", XPID_PLATFORM_DASH_PREFIX);
    ok = false;
  }
  if (!ValidateBytes(base, XPID_PLATFORM_COMPACT_NAME, kPsnShort, sizeof(kPsnShort))) {
    Log(EchoVR::LogLevel::Error,
        "[NEVR.XPID] Compact name mismatch at RVA 0x%X — expected \"PSN\\0\"", XPID_PLATFORM_COMPACT_NAME);
    ok = false;
  }

  if (!ok) {
    Log(EchoVR::LogLevel::Error, "[NEVR.XPID] Aborting DSC provider patch — prologue validation failed");
    return;
  }

  // Apply all three patches.
  static_assert(sizeof(kDscShort) == XPID_PLATFORM_SHORT_NAME_SIZE);
  static_assert(sizeof(kDscDash)  == XPID_PLATFORM_DASH_PREFIX_SIZE);
  static_assert(sizeof(kDscShort) == XPID_PLATFORM_COMPACT_NAME_SIZE);

  ApplyPatch(XPID_PLATFORM_SHORT_NAME,  kDscShort, sizeof(kDscShort));
  ApplyPatch(XPID_PLATFORM_DASH_PREFIX, kDscDash,  sizeof(kDscDash));
  ApplyPatch(XPID_PLATFORM_COMPACT_NAME, kDscShort, sizeof(kDscShort));

  Log(EchoVR::LogLevel::Info, "[NEVR.XPID] DSC provider patch applied (PSN- → DSC- at 3 sites)");
}
