#include "runtime/patch/xpid_patch.h"

#include "runtime/hook/patching.h"
#include "core/logging.h"
#include "core/globals.h"
#include "runtime/hook/addresses.h"
// N120. This runs from initialize.cpp BEFORE the built-in log filter is up, and
// its Log() output reached neither the console log nor nevr-boot.jsonl — so
// across every captured run the patch reported NOTHING, success or failure, and
// whether the game's provider strings were actually rewritten was unknowable.
// (`login injected xpid=DSC-...` does not prove it: ws_bridge builds that prefix
// itself, so it is green even when this patch never ran.) Tee the outcome.
#include "runtime/log/boot_log_tee.h"
#include "runtime/lifecycle/initialize.h"  // g_bootHookFailed

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

/// Report a mismatch with both expected and actual bytes — `ValidateBytes`
/// only returns a bool, so this re-derives `site` to show what was actually
/// found, not just what was expected. Warning, not Error: these are
/// diagnostic detail feeding into the single g_bootHookFailed decision below,
/// which boot.cpp turns into the terminal severity once the CLI says whether
/// this is a server — five unconditional Error lines here would read as five
/// separate decisions when it's really one.
static void LogByteMismatch(const CHAR* base, uintptr_t offset, const BYTE* expected, const char* what) {
  const BYTE* site = reinterpret_cast<const BYTE*>(base + offset);
  Log(EchoVR::LogLevel::Warning,
      "[NEVR.XPID] %s mismatch rva=0x%X expected=%02x%02x%02x%02x actual=%02x%02x%02x%02x",
      what, static_cast<unsigned>(offset),
      expected[0], expected[1], expected[2], expected[3],
      site[0], site[1], site[2], site[3]);
}

VOID PatchDscProvider() {
  using namespace nevr_patch_addresses;
  const CHAR* base = EchoVR::g_GameBaseAddress;

  // Validate all four sites before patching any.
  bool ok = true;
  if (!ValidateBytes(base, XPID_PLATFORM_SHORT_NAME, kPsnShort, sizeof(kPsnShort))) {
    LogByteMismatch(base, XPID_PLATFORM_SHORT_NAME, kPsnShort, "short name");
    ok = false;
  }
  if (!ValidateBytes(base, XPID_PLATFORM_DASH_PREFIX, kPsnDash, sizeof(kPsnDash))) {
    LogByteMismatch(base, XPID_PLATFORM_DASH_PREFIX, kPsnDash, "dash prefix");
    ok = false;
  }
  if (!ValidateBytes(base, XPID_PLATFORM_COMPACT_NAME, kPsnShort, sizeof(kPsnShort))) {
    LogByteMismatch(base, XPID_PLATFORM_COMPACT_NAME, kPsnShort, "compact name");
    ok = false;
  }
  if (!ValidateBytes(base, XPID_PLATFORM_FALLBACK_PREFIX, kQmarkDash, sizeof(kQmarkDash))) {
    LogByteMismatch(base, XPID_PLATFORM_FALLBACK_PREFIX, kQmarkDash, "fallback prefix");
    ok = false;
  }
  if (!ValidateBytes(base, XPID_PLATFORM_COMPACT_FALLBACK_NAME, kQmarkNull, sizeof(kQmarkNull))) {
    LogByteMismatch(base, XPID_PLATFORM_COMPACT_FALLBACK_NAME, kQmarkNull, "compact fallback name");
    ok = false;
  }

  if (!ok) {
    // No standalone "Aborting..." Log() line here: the failure is reported once,
    // through the boot tee below, and g_bootHookFailed carries the decision.
    nevr_boot_log_tee::TeeFprintf("[NEVR.XPID] validation FAILED — provider strings stay PSN-/?\?\?-\n");
    // N120. These five sites are validated against literal bytes in the loaded
    // image, so a mismatch means the binary is not the build this runtime targets.
    // Every other address in addresses.h is then suspect too — continuing would
    // apply patches derived from a different build, and the first visible symptom
    // would be somewhere unrelated. Verified safe to make fatal: a real server run
    // reports "DSC provider patch applied at 5 sites", so this fires only on an
    // actual binary mismatch, never on a healthy boot.
    //
    // ServerFatal is not callable here: this runs from Initialize() before the CLI
    // is parsed, so g_isServer is still false and ServerFatal would only warn.
    // g_bootHookFailed is checked by boot.cpp once g_isServer is known, and a
    // server refuses to start with it set (a client reports ok=false and goes on).
    g_bootHookFailed = true;
    nevr_boot_log_tee::TeeFprintf(
        "[NEVR.XPID] XPID provider-string validation failed — echovr.exe is not the build this runtime "
        "targets; every patched address is suspect; a server will refuse to start\n");
    return;
  }

  // Apply all five patches.
  static_assert(sizeof(kDscShort) == XPID_PLATFORM_SHORT_NAME_SIZE);
  static_assert(sizeof(kDscDash)  == XPID_PLATFORM_DASH_PREFIX_SIZE);
  static_assert(sizeof(kDscShort) == XPID_PLATFORM_COMPACT_NAME_SIZE);
  static_assert(sizeof(kDscDash)  == XPID_PLATFORM_FALLBACK_PREFIX_SIZE);
  static_assert(sizeof(kDscShort) == XPID_PLATFORM_COMPACT_FALLBACK_NAME_SIZE);

  bool allPatched = true;
  allPatched &= ApplyPatch(XPID_PLATFORM_SHORT_NAME,  kDscShort, sizeof(kDscShort));
  allPatched &= ApplyPatch(XPID_PLATFORM_DASH_PREFIX, kDscDash,  sizeof(kDscDash));
  allPatched &= ApplyPatch(XPID_PLATFORM_COMPACT_NAME, kDscShort, sizeof(kDscShort));
  allPatched &= ApplyPatch(XPID_PLATFORM_FALLBACK_PREFIX, kDscDash, sizeof(kDscDash));
  allPatched &= ApplyPatch(XPID_PLATFORM_COMPACT_FALLBACK_NAME, kDscShort, sizeof(kDscShort));

  if (!allPatched) {
    // Same rule as the validation failure above: no ServerFatal before the CLI is
    // parsed. The identity strings are now a mix of PSN-/DSC-, so a server must not run.
    g_bootHookFailed = true;
    nevr_boot_log_tee::TeeFprintf(
        "[NEVR.XPID] provider-string patch FAILED — identity strings are inconsistent; "
        "a server will refuse to start\n");
    return;
  }

  Log(EchoVR::LogLevel::Info, "[NEVR.XPID] DSC provider patch applied (PSN-/?\?- → DSC- at 5 sites)");
  nevr_boot_log_tee::TeeFprintf("[NEVR.XPID] DSC provider patch applied at 5 sites\n");
}

// ============================================================================
// GetProviderPrefix detour — one of two xpid choke-points (GetUserIDString, which
// has its own switch and 22 distinct callers, is not hooked; see addresses.h)
// ============================================================================

typedef void* (*GetProviderPrefixFn)(uint32_t* providerBits);
static GetProviderPrefixFn g_RealGetProviderPrefix = nullptr;

static void* GetProviderPrefixHook(uint32_t* /*providerBits*/) {
  // Always return the OVR-ORG string-table pointer.  The xpids built through
  // this function — CreateUser, SaveLocalData, LobbyFindSession,
  // LobbyPlayerSessions, three Send() paths, and 7 more callers — all carry
  // the same prefix without touching any string table or CNSUser nibble.
  // GetUserIDString's callers are not affected.
  return EchoVR::g_GameBaseAddress + nevr_patch_addresses::PROVIDER_STRING_OVR_ORG;
}

VOID PatchProviderPrefixOvrOrg() {
  using namespace nevr_patch_addresses;
  void* target = EchoVR::g_GameBaseAddress + GET_PROVIDER_PREFIX;
  g_RealGetProviderPrefix = reinterpret_cast<GetProviderPrefixFn>(target);
  if (PatchDetour(&g_RealGetProviderPrefix, reinterpret_cast<PVOID>(GetProviderPrefixHook),
                  "GetProviderPrefix")) {
    Log(EchoVR::LogLevel::Info, "[NEVR.XPID] GetProviderPrefix detoured → OVR-ORG (17 distinct callers)");
    nevr_boot_log_tee::TeeFprintf("[NEVR.XPID] GetProviderPrefix detour OK\n");
  } else {
    Log(EchoVR::LogLevel::Error, "[NEVR.XPID] GetProviderPrefix detour failed target=%p status=%s",
        target, Hooking::LastAttachError());
    nevr_boot_log_tee::TeeFprintf("[NEVR.XPID] GetProviderPrefix detour FAILED: %s\n",
                           Hooking::LastAttachError());
  }
}
