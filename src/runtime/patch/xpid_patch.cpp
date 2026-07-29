#include "runtime/patch/xpid_patch.h"

#include "runtime/hook/patching.h"
#include "core/logging.h"
#include "runtime/hook/addresses.h"
// N120. This runs from initialize.cpp BEFORE the built-in log filter is up, and
// its Log() output reached neither the console log nor nevr-boot.jsonl — so
// across every captured run the patch reported NOTHING, success or failure, and
// whether the game's provider strings were actually rewritten was unknowable.
// (`login injected xpid=DSC-...` does not prove it: ws_bridge builds that prefix
// itself, so it is green even when this patch never ran.) Tee the outcome.
#include "runtime/log/boot_log_tee.h"
#include "runtime/lifecycle/crash_recovery.h"  // ServerFatal

// Expected original bytes at each patch site (for validation).
static const BYTE kPsnShort[] = {0x50, 0x53, 0x4E, 0x00};  // "PSN\0"
static const BYTE kPsnDash[]  = {0x50, 0x53, 0x4E, 0x2D};  // "PSN-"
static const BYTE kQmarkDash[] = {0x3F, 0x3F, 0x3F, 0x2D};  // "???-"
static const BYTE kQmarkNull[] = {0x3F, 0x3F, 0x3F, 0x00};  // "???\0"

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

  // Validate all four sites before patching any.
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
  if (!ValidateBytes(base, XPID_PLATFORM_FALLBACK_PREFIX, kQmarkDash, sizeof(kQmarkDash))) {
    Log(EchoVR::LogLevel::Error,
        "[NEVR.XPID] Fallback prefix mismatch at RVA 0x%X — expected \"?\?\?-\"", XPID_PLATFORM_FALLBACK_PREFIX);
    ok = false;
  }
  if (!ValidateBytes(base, XPID_PLATFORM_COMPACT_FALLBACK_NAME, kQmarkNull, sizeof(kQmarkNull))) {
    Log(EchoVR::LogLevel::Error,
        "[NEVR.XPID] Compact fallback name mismatch at RVA 0x%X — expected \"???\\0\"", XPID_PLATFORM_COMPACT_FALLBACK_NAME);
    ok = false;
  }

  if (!ok) {
    Log(EchoVR::LogLevel::Error, "[NEVR.XPID] Aborting DSC provider patch — prologue validation failed");
    BootLogTee::TeeFprintf("[NEVR.XPID] validation FAILED — provider strings stay PSN-/?\?\?-\n");
    // N120. These five sites are validated against literal bytes in the loaded
    // image, so a mismatch means the binary is not the build this runtime targets.
    // Every other address in addresses.h is then suspect too — continuing would
    // apply patches derived from a different build, and the first visible symptom
    // would be somewhere unrelated. Verified safe to make fatal: a real server run
    // reports "DSC provider patch applied at 5 sites", so this fires only on an
    // actual binary mismatch, never on a healthy boot.
    ServerFatal("XPID provider-string validation failed — echovr.exe is not the "
                "build this runtime targets; every patched address is suspect");
    return;
  }

  // Apply all five patches.
  static_assert(sizeof(kDscShort) == XPID_PLATFORM_SHORT_NAME_SIZE);
  static_assert(sizeof(kDscDash)  == XPID_PLATFORM_DASH_PREFIX_SIZE);
  static_assert(sizeof(kDscShort) == XPID_PLATFORM_COMPACT_NAME_SIZE);
  static_assert(sizeof(kDscDash)  == XPID_PLATFORM_FALLBACK_PREFIX_SIZE);
  static_assert(sizeof(kDscShort) == XPID_PLATFORM_COMPACT_FALLBACK_NAME_SIZE);

  ApplyPatch(XPID_PLATFORM_SHORT_NAME,  kDscShort, sizeof(kDscShort));
  ApplyPatch(XPID_PLATFORM_DASH_PREFIX, kDscDash,  sizeof(kDscDash));
  ApplyPatch(XPID_PLATFORM_COMPACT_NAME, kDscShort, sizeof(kDscShort));
  ApplyPatch(XPID_PLATFORM_FALLBACK_PREFIX, kDscDash, sizeof(kDscDash));
  ApplyPatch(XPID_PLATFORM_COMPACT_FALLBACK_NAME, kDscShort, sizeof(kDscShort));

  Log(EchoVR::LogLevel::Info, "[NEVR.XPID] DSC provider patch applied (PSN-/?\?- → DSC- at 5 sites)");
  BootLogTee::TeeFprintf("[NEVR.XPID] DSC provider patch applied at 5 sites\n");
}
