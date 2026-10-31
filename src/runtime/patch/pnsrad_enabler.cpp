/* SYNTHESIS -- custom tool code, not from binary */

/* ======================================================================
 * pnsrad_enabler — Force pnsrad.dll as the social platform
 *
 * EchoVR selects between social platform DLLs ("pnsovr", "pnsdemo", "pnsrad")
 * via flag-driven logic. This module:
 *
 *   1. Redirects the provider loader's "pnsovr" and "pnsdemo" names to
 *      "pnsrad" (runtime/patch/provider_name_redirect.h), so all code paths
 *      load pnsrad.dll regardless of flags.
 *
 *   2. NOPs the OVR platform branch so PlatformModuleDecisionAndInitialize
 *      takes Path 2 (full networking).
 *
 *   3. Patches pnsrad.dll's login provider check when it loads. The loader's
 *      DLL notification only records the load (it runs under the loader lock);
 *      the patches land when the game's LoadLibraryW returns, before the game
 *      resolves any export of the module (docs/reference/pnsrad-load-boundary.md).
 *
 *   4. NOPs LogInSuccessCB's identity comparison so it processes LoginSuccess
 *      even with uninitialized local identity.
 *
 *   5. NOPs LoginIdResponseCB's authenticated state flag check so GameSettings
 *      are processed even when the injected LoginRequest bypasses pnsrad's
 *      state machine.
 *
 *   6. Makes the exported UserProviderID return the "OVR" provider symbol so the
 *      game's provider checks (friend invite handler, party member callbacks)
 *      agree with the OVR-ORG identity (runtime/patch/provider_identity.h).
 * ====================================================================== */

#include "runtime/patch/pnsrad_enabler.h"
#include "runtime/patch/provider_identity.h"
#include "runtime/patch/provider_name_redirect.h"
#include "runtime/lifecycle/crash_recovery.h"  // ServerFatal: mode-gated (fatal on a server, a Warning on a client)
#include "runtime/patch/pnsrad_diag_queue.h"
#include "runtime/patch/pnsrad_load_queue.h"
#include "runtime/hook/dll_load_hook.h"
#include "runtime/hook/process_memory.h"
#include "runtime/hook/patching.h"
#include "runtime/compat/ws_bridge.h"  // GetMatchmakerBridgePort()
#include "runtime/compat/self_check.h"
#include "runtime/patch/matchmaker_host_patch.h"
#include "core/logging.h"
#include "nevr/pc/nevr_common.h"      // N97: the one ValidatePrologue

#include <cstdio>
#include <cstring>
#include <string>
#include <atomic>
#include <mutex>

#ifdef _WIN32
#include <windows.h>
#include <winternl.h>
#include <MinHook.h>
#endif

#ifdef _WIN32

/* --------------------------------------------------------------------
 * Patch addresses (RVA from image base)
 * -------------------------------------------------------------------- */

// OVR platform branch — 6-byte JNE in PlatformModuleDecisionAndInitialize
static constexpr uintptr_t OVR_BRANCH = 0x1580e5;
static constexpr uint8_t   OVR_JNE_EXPECTED[] = {0x0F, 0x85, 0xC7, 0x00, 0x00, 0x00};

// pnsrad.dll login provider check — JNE at CNSRADUser::vfunction1+0x13
static constexpr uintptr_t PNSRAD_LOGIN_CHECK = 0x85b53;
static constexpr uint8_t   PNSRAD_JNE_EXPECTED[] = {0x75, 0x1f};

// pnsrad.dll LoginIdResponseCB state check — JE at FUN_18008f140+0x76
static constexpr uintptr_t PNSRAD_LOGIN_STATE_CHECK = 0x8f1b6;
static constexpr uint8_t   PNSRAD_STATE_JE_EXPECTED[] = {0x0F, 0x84, 0x78, 0x01, 0x00, 0x00};

// pnsrad.dll LogInSuccessCB session/identity guard — JNE at FUN_18008eea0+0x85
static constexpr uintptr_t PNSRAD_LOGIN_IDENTITY_CHECK = 0x8ef25;
static constexpr uint8_t   PNSRAD_IDENTITY_JNE_EXPECTED[] = {0x0F, 0x85, 0x9C, 0x00, 0x00, 0x00};

// 2026-09-13 (Andrew): pnsradmatchmaking.dll's CNSRadMatchmaking::
// initialize_matchmakers reads "matchmaker_host" (default: the readyatdawn.com
// literal below) then "matchingservice_host" (defaults to whatever the first
// lookup returned) via its own statically-linked CJson::TString — a config
// read entirely separate from echovr.exe's JsonValueAsString, so our
// RedirectServiceUrl/config-override hook in config.cpp can never see or
// correct it. Measured live: even with config.json's "matchingservice_host"
// set correctly (ws://g.echovrce.com:80/spr), the matchmaker connection still
// went to the compiled default and got reset immediately (readyatdawn.com is
// dead) — confirms this DLL never found either key in whatever it reads.
// ws_bridge.cpp already binds a second listener for exactly this (N146
// comment there), but nothing connected to it because the game never
// tried — it was stuck on the compiled default.
//
// Load timing, confirmed live via a strace-style empirical test (renamed
// pnsradmatchmaking.dll aside, launched, watched the client log): it loads
// on demand at the lobby stage, well after login —
// "[EVR] [NSLOBBY] loading matchmaking library 'pnsradmatchmaking'" — via a
// native LoadLibrary-style path in cnslobby.cpp (confirmed by the clean
// "failed to load module ... Unable to load matchmaking library" error when
// the file was absent). The DLL notification records this load whenever it
// happens, and the patch lands when that LoadLibrary returns, the same path
// as pnsrad.dll's patches below.
//
// pnsradmatchmaking.dll, ImageBase 0x180000000 (confirmed via
// `objdump -p pnsradmatchmaking.dll | grep -i imagebase`):
//   RVA 0x1c84d8 (.rdata section, VMA 0x1801c6000, file offset 0x1c5200 per
//   `objdump -h`; string file offset = 0x1c76d8, verified byte-for-byte via
//   `dd if=pnsradmatchmaking.dll bs=1 skip=$((0x1c76d8)) count=64 | xxd`) —
//   a 48-byte slot (the 47-char string + NUL, then unrelated data
//   immediately follows with no padding) holding
//   "wss://matchmaker.readyatdawn.com/rad/rad15_live\0".
// Replacing with "ws://127.0.0.1:PPPPP\0" (21 bytes, port always 5 digits —
// ws_bridge.cpp draws from the 49152-65535 ephemeral range) leaves the tail
// of the original slot as inert bytes after our new NUL terminator — same
// pattern xpid_patch.cpp already uses for a shorter replacement in a fixed
// slot.
//
// The replacement is built from GetMatchmakerBridgePort() at call time: ws_bridge.cpp binds an
// ephemeral port with retry (a fixed port collides with a still-releasing socket from a just-killed
// process), so no compile-time constant exists.
static constexpr uintptr_t PNSRADMATCHMAKING_HOST_RVA = nevr_matchmaker_host_patch::kHostRva;
static constexpr size_t    PNSRADMATCHMAKING_HOST_SLOT_SIZE = nevr_matchmaker_host_patch::kHostSlotSize;
static constexpr const char* PNSRADMATCHMAKING_HOST_EXPECTED = nevr_matchmaker_host_patch::kHostExpected;

// 2026-09-13 (Andrew/ReVault audit): CNSRADParty (and, per the same audit,
// CNSRADFriends/CNSRADUsers/CNSRADActivities) register every inbound SNS
// listener via CTcpBroadcaster::Listen using a broadcaster HANDLE stored in
// the object itself — *(this+0x2b0) for CNSRADParty. If that handle is null
// at the moment this runs, every Listen() call it makes (InviteNotifyCB
// included) silently registers nothing — no error, no log — which would
// explain an invite with no listener evidence (see ReVault comments on
// 0x3039c4/libpnsrad.so and 0x180082d20/pnsrad.dll). The diagnostic observes
// the handle when registration occurs after its deferred installation.
//
// Target: the vslot[8] function that does the Listen() registration loop
// (pnsrad.dll FUN_180082d20, confirmed via ReVault to read the handle at
// param_1[0x56] == *(this+0x2b0) before every Listen call). RVA computed
// against pnsrad.dll's 0x180000000 image base (same convention used
// throughout this file for pnsradmatchmaking.dll's RVA above).
static constexpr uintptr_t PNSRAD_PARTY_INITIALIZE_RVA = 0x82d20;
static constexpr uintptr_t PNSRAD_PARTY_BROADCASTER_HANDLE_OFFSET = 0x2b0;

// The listener-registration hook does not fire during in-game invite attempts;
// registration may run before the deferred diagnostic hook is installed.
// SendInvite is the per-click function (ReVault FUN_180086df0, identified
// against libpnsrad.so) and reads the same broadcaster-handle field.
static constexpr uintptr_t PNSRAD_PARTY_SEND_INVITE_RVA = 0x86df0;

/* --------------------------------------------------------------------
 * Memory patching
 * -------------------------------------------------------------------- */

/// Lowercase hex dump — same shape as asset_cdn.cpp's BytesToHex, kept local
/// since this is the only file in this pair that needs it for variable-length
/// (2 or 6 byte) expected/actual prologue dumps.
static std::string BytesToHex(const uint8_t* bytes, size_t len) {
    static const char hex[] = "0123456789abcdef";
    std::string result;
    result.reserve(len * 2);
    for (size_t i = 0; i < len; i++) {
        result.push_back(hex[bytes[i] >> 4]);
        result.push_back(hex[bytes[i] & 0x0F]);
    }
    return result;
}

/* --------------------------------------------------------------------
 * LdrDllNotification — patches pnsrad.dll when it loads
 * -------------------------------------------------------------------- */

typedef struct _LDR_DLL_NOTIFICATION_DATA {
    ULONG Flags;
    const UNICODE_STRING* FullDllName;
    const UNICODE_STRING* BaseDllName;
    PVOID DllBase;
    ULONG SizeOfImage;
} LDR_DLL_NOTIFICATION_DATA;

typedef void (CALLBACK *LDR_DLL_NOTIFICATION_FUNCTION)(ULONG reason,
    const LDR_DLL_NOTIFICATION_DATA* data, void* context);
typedef NTSTATUS (NTAPI *LdrRegisterDllNotification_fn)(ULONG flags,
    LDR_DLL_NOTIFICATION_FUNCTION func, void* context, void** cookie);
typedef NTSTATUS (NTAPI *LdrUnregisterDllNotification_fn)(void* cookie);

static void* s_dllNotifCookie = nullptr;
static std::atomic<bool> s_pnsradPatched{false};

/* Self-check "matchmaking_reload_patch" (#18, docs/engine/remote-log.md): the game loads
 * pnsradmatchmaking.dll once per lobby and every load must have its host default re-applied. The apply
 * step counts each load; the probe reads the counts on the frame tick. */
static nevr_matchmaker_host_patch::ReloadLedger s_mmLedger;

static bool MatchmakingReloadProbe(nevr_self_check::Observation* out) {
    return s_mmLedger.Take(&out->observed, &out->pass);
}

/* Registered from Init (the game's boot, not the DLL's static initialisation: Register allocates). */
static void RegisterMatchmakingReloadCheck() {
    nevr_self_check::Register(
        {"matchmaking_reload_patch",
         "every pnsradmatchmaking.dll load had its matchmaker host default re-applied (patched == loads)",
         &MatchmakingReloadProbe});
}
static std::atomic<uintptr_t> s_pnsradModuleBase{0};
static PnsradDiagQueue s_pendingDiagnostics;
/* Loads the DLL notification recorded and the apply step has not patched yet. */
static nevr_pnsrad_load::PendingLoads s_pendingLoads;
/* One apply at a time: a LoadLibrary that returns while another thread is patching waits here, so it cannot
 * hand the caller a module whose patches are still being written. */
static std::mutex s_applyMutex;

/* Patch pnsradmatchmaking.dll's compiled matchmaker-host default so the
 * matchmaker connection reaches our own listener instead of the dead
 * readyatdawn.com host. See PNSRADMATCHMAKING_HOST_RVA's comment above for
 * the full measurement (RVA, file offset, slot size, exact original bytes). */
static void ReportMatchmakingHostResult(nevr_matchmaker_host_patch::Result result, const uint8_t* image,
                                        uint16_t port, DWORD err) {
    switch (result) {
    case nevr_matchmaker_host_patch::Result::Patched:
        Log(EchoVR::LogLevel::Info,
            "[NEVR.PATCH] pnsradmatchmaking patched matchmaker host default at +0x%x: "
            "\"%s\" -> \"ws://127.0.0.1:%u\"", (unsigned)PNSRADMATCHMAKING_HOST_RVA,
            PNSRADMATCHMAKING_HOST_EXPECTED, static_cast<unsigned>(port));
        break;
    case nevr_matchmaker_host_patch::Result::NoPort:
        Log(EchoVR::LogLevel::Warning,
            "[NEVR.PATCH] pnsradmatchmaking matchmaker listener never bound a port — NOT "
            "patching (matchmaking will fail regardless)");
        break;
    case nevr_matchmaker_host_patch::Result::DoesNotFit:
        Log(EchoVR::LogLevel::Warning,
            "[NEVR.PATCH] pnsradmatchmaking replacement does not fit the %zu-byte slot at +0x%x — NOT patched",
            PNSRADMATCHMAKING_HOST_SLOT_SIZE, static_cast<unsigned>(PNSRADMATCHMAKING_HOST_RVA));
        break;
    case nevr_matchmaker_host_patch::Result::BytesMismatch:
        Log(EchoVR::LogLevel::Warning,
            "[NEVR.PATCH] pnsradmatchmaking host patch skipped rva=0x%x reason=bytes_mismatch "
            "expected=\"%s\" actual=\"%.48s\"",
            static_cast<unsigned>(PNSRADMATCHMAKING_HOST_RVA), PNSRADMATCHMAKING_HOST_EXPECTED,
            reinterpret_cast<const char*>(image + PNSRADMATCHMAKING_HOST_RVA));
        break;
    case nevr_matchmaker_host_patch::Result::WriteFailed:
        Log(EchoVR::LogLevel::Warning,
            "[NEVR.PATCH] pnsradmatchmaking PatchMemory FAILED rva=0x%x error=%lu — bytes matched "
            "but the write did not land", (unsigned)PNSRADMATCHMAKING_HOST_RVA, err);
        break;
    }
}

/* --------------------------------------------------------------------
 * Diagnostic: is CNSRADParty's broadcaster handle null when it registers
 * its SNS listeners? See PNSRAD_PARTY_INITIALIZE_RVA's comment above.
 * -------------------------------------------------------------------- */

typedef uint64_t (*PartyListenerRegisterFn)(void* thisPtr, uint64_t param2);
static PartyListenerRegisterFn g_RealPartyListenerRegister = nullptr;

static uint64_t PartyListenerRegisterHook(void* thisPtr, uint64_t param2) {
    uintptr_t handle = 0;
    if (thisPtr) {
        handle = *reinterpret_cast<uintptr_t*>(
            reinterpret_cast<uint8_t*>(thisPtr) + PNSRAD_PARTY_BROADCASTER_HANDLE_OFFSET);
    }
    // Resolved summary at INFO (Rule/Hard-Stop: raw pointer values are DEBUG-
    // only); the raw pointers themselves, for RE correlation, go out at DEBUG.
    Log(EchoVR::LogLevel::Info,
        "[NEVR.PATCH] pnsrad DIAG party listener-register: broadcaster_handle=%s",
        handle == 0 ? "NULL" : "non-null");
    Log(EchoVR::LogLevel::Debug,
        "[NEVR.PATCH] pnsrad DIAG party listener-register this=%p broadcaster_handle=%p",
        thisPtr, reinterpret_cast<void*>(handle));
    return g_RealPartyListenerRegister(thisPtr, param2);
}

static void InstallPartyBroadcasterDiag(uintptr_t base) {
    void* target = reinterpret_cast<void*>(base + PNSRAD_PARTY_INITIALIZE_RVA);
    g_RealPartyListenerRegister = reinterpret_cast<PartyListenerRegisterFn>(target);
    if (PatchDetour(&g_RealPartyListenerRegister,
                    reinterpret_cast<PVOID>(PartyListenerRegisterHook), "pnsrad party listener-register")) {
        Log(EchoVR::LogLevel::Info,
            "[NEVR.PATCH] pnsrad DIAG party-broadcaster hook installed at +0x%x",
            (unsigned)PNSRAD_PARTY_INITIALIZE_RVA);
    } else {
        Log(EchoVR::LogLevel::Warning,
            "[NEVR.PATCH] pnsrad DIAG party-broadcaster hook FAILED at +0x%x: %s",
            (unsigned)PNSRAD_PARTY_INITIALIZE_RVA, Hooking::LastAttachError());
    }
}

typedef void (*PartySendInviteFn)(void* thisPtr, uint64_t targetAccountId);
static PartySendInviteFn g_RealPartySendInvite = nullptr;

static void PartySendInviteHook(void* thisPtr, uint64_t targetAccountId) {
    uintptr_t handle = 0;
    if (thisPtr) {
        handle = *reinterpret_cast<uintptr_t*>(
            reinterpret_cast<uint8_t*>(thisPtr) + PNSRAD_PARTY_BROADCASTER_HANDLE_OFFSET);
    }
    Log(EchoVR::LogLevel::Info,
        "[NEVR.PATCH] pnsrad DIAG party send-invite: target=%llu broadcaster_handle=%s",
        (unsigned long long)targetAccountId, handle == 0 ? "NULL" : "non-null");
    Log(EchoVR::LogLevel::Debug,
        "[NEVR.PATCH] pnsrad DIAG party send-invite this=%p target=%llu broadcaster_handle=%p",
        thisPtr, (unsigned long long)targetAccountId, reinterpret_cast<void*>(handle));
    g_RealPartySendInvite(thisPtr, targetAccountId);
}

static void InstallPartySendInviteDiag(uintptr_t base) {
    void* target = reinterpret_cast<void*>(base + PNSRAD_PARTY_SEND_INVITE_RVA);
    g_RealPartySendInvite = reinterpret_cast<PartySendInviteFn>(target);
    if (PatchDetour(&g_RealPartySendInvite,
                    reinterpret_cast<PVOID>(PartySendInviteHook), "pnsrad party send-invite")) {
        Log(EchoVR::LogLevel::Info,
            "[NEVR.PATCH] pnsrad DIAG party-send-invite hook installed at +0x%x",
            (unsigned)PNSRAD_PARTY_SEND_INVITE_RVA);
    } else {
        Log(EchoVR::LogLevel::Warning,
            "[NEVR.PATCH] pnsrad DIAG party-send-invite hook FAILED at +0x%x: %s",
            (unsigned)PNSRAD_PARTY_SEND_INVITE_RVA, Hooking::LastAttachError());
    }
}

/* Patch accounting (2026-07-26).
 *
 * These three patches logged success at Debug and prologue-mismatch at Warning,
 * and logged NOTHING AT ALL when PatchMemory itself failed. With min_level=INFO
 * that makes a fully-successful run and a never-fired callback look identical in
 * the log — the N86 failure mode, and the reason "is pnsrad enabled?" could not
 * be answered from a server log.
 *
 * One aggregate at Info, in the shape N17 defined for hook installs. */
static int s_pnsradOk = 0;
static int s_pnsradFail = 0;

/* Apply one NOP patch, counting and reporting every outcome including the
 * PatchMemory failure. */
static void PnsradNopPatch(uint8_t* site, const uint8_t* expected, size_t expLen,
                           size_t nopLen, const char* what, unsigned rva) {
    if (!nevr::ValidatePrologue(site, expected, expLen)) {
        Log(EchoVR::LogLevel::Warning,
            "[NEVR.PATCH] pnsrad %s patch skipped rva=0x%x reason=bytes_mismatch expected=%s actual=%s",
            what, rva, BytesToHex(expected, expLen).c_str(), BytesToHex(site, expLen).c_str());
        s_pnsradFail++;
        return;
    }
    uint8_t nops[8];
    for (size_t i = 0; i < nopLen && i < sizeof(nops); i++) nops[i] = 0x90;
    DWORD err = 0;
    if (ProcessMemcpy(site, nops, nopLen, &err)) {
        Log(EchoVR::LogLevel::Debug, "[NEVR.PATCH] pnsrad patched %s at +0x%x", what, rva);
        s_pnsradOk++;
    } else {
        Log(EchoVR::LogLevel::Warning,
            "[NEVR.PATCH] pnsrad PatchMemory FAILED %s rva=0x%x error=%lu — prologue matched but the "
            "write did not land", what, rva, err);
        s_pnsradFail++;
    }
}

/* Replace pnsrad's UserProviderID body so it returns the OVR provider symbol
 * (runtime/patch/provider_identity.h has the measurement). Same accounting as the
 * NOP patches: validated first, every outcome counted and reported. */
static void PnsradUserProviderIdPatch(uintptr_t base) {
    using namespace nevr_provider_identity;
    uint8_t* site = reinterpret_cast<uint8_t*>(base + kPnsradUserProviderIdRva);
    if (!nevr::ValidatePrologue(site, kPnsradUserProviderIdExpected.data(), kPnsradUserProviderIdExpected.size())) {
        Log(EchoVR::LogLevel::Warning,
            "[NEVR.PATCH] pnsrad UserProviderID patch skipped rva=0x%x reason=bytes_mismatch expected=%s actual=%s",
            static_cast<unsigned>(kPnsradUserProviderIdRva),
            BytesToHex(kPnsradUserProviderIdExpected.data(), kPnsradUserProviderIdExpected.size()).c_str(),
            BytesToHex(site, kPnsradUserProviderIdExpected.size()).c_str());
        s_pnsradFail++;
        return;
    }
    const auto code = ReturnConstant(kOvrProviderSymbol);
    DWORD err = 0;
    if (ProcessMemcpy(site, code.data(), code.size(), &err)) {
        Log(EchoVR::LogLevel::Debug, "[NEVR.PATCH] pnsrad UserProviderID now returns OVR at +0x%x",
            static_cast<unsigned>(kPnsradUserProviderIdRva));
        s_pnsradOk++;
    } else {
        Log(EchoVR::LogLevel::Warning,
            "[NEVR.PATCH] pnsrad PatchMemory FAILED UserProviderID rva=0x%x error=%lu — bytes matched but the "
            "write did not land", static_cast<unsigned>(kPnsradUserProviderIdRva), err);
        s_pnsradFail++;
    }
}

static void ApplyPnsradLoad(uintptr_t base) {
    // pnsrad.dll is loaded once per process; a second record is the same image or a reload this runtime
    // does not expect, and its patches were already counted.
    if (s_pnsradPatched.exchange(true)) {
        Log(EchoVR::LogLevel::Warning,
            "[NEVR.PATCH] pnsrad.dll loaded a second time — patches not re-applied (pnsrad.dll is expected to "
            "load once; the login patches cover the first image only)");
        return;
    }
    s_pnsradModuleBase.store(base, std::memory_order_release);

    PnsradNopPatch(reinterpret_cast<uint8_t*>(base + PNSRAD_LOGIN_CHECK),
                   PNSRAD_JNE_EXPECTED, sizeof(PNSRAD_JNE_EXPECTED), 2,
                   "login check", (unsigned)PNSRAD_LOGIN_CHECK);
    PnsradNopPatch(reinterpret_cast<uint8_t*>(base + PNSRAD_LOGIN_IDENTITY_CHECK),
                   PNSRAD_IDENTITY_JNE_EXPECTED, sizeof(PNSRAD_IDENTITY_JNE_EXPECTED), 6,
                   "identity guard", (unsigned)PNSRAD_LOGIN_IDENTITY_CHECK);
    PnsradNopPatch(reinterpret_cast<uint8_t*>(base + PNSRAD_LOGIN_STATE_CHECK),
                   PNSRAD_STATE_JE_EXPECTED, sizeof(PNSRAD_STATE_JE_EXPECTED), 6,
                   "state check", (unsigned)PNSRAD_LOGIN_STATE_CHECK);
    PnsradUserProviderIdPatch(base);
    // Optional diagnostics: MinHook suspends every other thread, so the frame tick installs them.
    s_pendingDiagnostics.NoteLoaded(base);

    Log(EchoVR::LogLevel::Info,
        "[NEVR.PATCH] pnsrad module patches: %d succeeded, %d failed — social layer "
        "(friends/party/login) %s",
        s_pnsradOk, s_pnsradFail,
        (s_pnsradFail == 0 && s_pnsradOk == 4) ? "ENABLED"
        : (s_pnsradOk == 0) ? "NOT PATCHED"
                            : "PARTIALLY PATCHED");
}

static void ApplyMatchmakingLoad(uintptr_t base) {
    // No once-only guard: the game unloads and reloads this DLL, and every load maps a fresh, unpatched image.
    // The matchmaker listener binds at early boot (InstallWebSocketBridge) and this DLL loads at the lobby
    // stage, so the port is already chosen. That ordering is observed, not enforced; a zero port is reported.
    const uint16_t port = GetMatchmakerBridgePort();
    auto* image = reinterpret_cast<uint8_t*>(base);
    DWORD err = 0;
    const auto result = nevr_matchmaker_host_patch::Apply(
        image, port, [&err](uint8_t* dst, const char* src, size_t len) { return ProcessMemcpy(dst, src, len, &err); });
    ReportMatchmakingHostResult(result, image, port, err);
    s_mmLedger.NoteLoad(result);
}

/* Where a recorded load was patched. LibraryReturn is the proven boundary: the game's LoadLibraryW returned
 * and the game has not yet resolved an export. FrameTick is the fallback, for a record at least one tick old: a
 * load that did not pass through a hooked LoadLibrary* (or returned while its thread held the loader lock), which
 * the game may already have run. */
enum class ApplyPoint { LibraryReturn, FrameTick };

static bool ModuleStillMappedAt(uintptr_t base) {
    HMODULE module = nullptr;
    return GetModuleHandleExW(GET_MODULE_HANDLE_EX_FLAG_FROM_ADDRESS | GET_MODULE_HANDLE_EX_FLAG_UNCHANGED_REFCOUNT,
                              reinterpret_cast<LPCWSTR>(base), &module) &&
           reinterpret_cast<uintptr_t>(module) == base;
}

static size_t ApplyPendingLoads(ApplyPoint at) {
    std::lock_guard<std::mutex> lock(s_applyMutex);
    const uint64_t dropped = s_pendingLoads.TakeDropped();
    if (dropped != 0) {
        Log(EchoVR::LogLevel::Error,
            "[NEVR.PATCH] pnsrad load queue full: %llu load(s) of pnsrad.dll or pnsradmatchmaking.dll were not "
            "recorded and stay unpatched (login or matchmaking fails for them)",
            static_cast<unsigned long long>(dropped));
    }
    const auto apply = [at](nevr_pnsrad_load::Module module, uintptr_t base) {
        const bool pnsrad = module == nevr_pnsrad_load::Module::Pnsrad;
        const char* name = pnsrad ? nevr_pnsrad_load::kPnsradName : nevr_matchmaker_host_patch::kModuleName;
        if (!ModuleStillMappedAt(base)) {
            Log(EchoVR::LogLevel::Warning,
                "[NEVR.PATCH] %s unloaded before its patches were applied (apply_point=%s) — nothing left to patch",
                name, at == ApplyPoint::LibraryReturn ? "library_return" : "frame_tick");
            if (!pnsrad) s_mmLedger.NoteLoad(std::nullopt);
            return;
        }
        if (at == ApplyPoint::FrameTick) {
            Log(EchoVR::LogLevel::Warning,
                "[NEVR.PATCH] %s patched at the frame tick, not when its LoadLibrary returned — the load did not "
                "pass through a hooked LoadLibrary*, so the game may have used the module unpatched",
                name);
        }
        if (pnsrad) {
            ApplyPnsradLoad(base);
        } else {
            ApplyMatchmakingLoad(base);
        }
    };
    if (at == ApplyPoint::LibraryReturn) return s_pendingLoads.Drain(apply);
    return s_pendingLoads.DrainNotedBefore(s_pendingLoads.AdvanceEpoch(), apply);
}

/* DllLoadHook post-load observer: runs on the thread whose LoadLibrary* call just returned from the loader,
 * outside the loader lock, before that call returns to the game. */
static void ApplyPendingLoadsAtLibraryReturn() { ApplyPendingLoads(ApplyPoint::LibraryReturn); }

/* LdrDllNotification callback. It runs under the loader lock, where calling into another module is unsafe,
 * so it records the load and returns: no memory write, no log, no call out of this module. */
static void CALLBACK OnDllLoaded(ULONG reason, const LDR_DLL_NOTIFICATION_DATA* data, void*) {
    // Only loads act (an unload leaves nothing to undo: the image and its patch are gone together).
    if (reason != nevr_matchmaker_host_patch::kNotificationLoaded || !data || !data->BaseDllName || !data->DllBase) {
        return;
    }
    const UNICODE_STRING* name = data->BaseDllName;
    const auto module = nevr_pnsrad_load::Classify(name->Buffer, name->Length / sizeof(WCHAR));
    if (module) s_pendingLoads.Note(*module, reinterpret_cast<uintptr_t>(data->DllBase));
}

#endif // _WIN32

/* ====================================================================
 * Public API
 * ==================================================================== */

void nevr_pnsrad_enabler::Init(uintptr_t base_addr) {
    RegisterMatchmakingReloadCheck();
#ifdef _WIN32
    // Total echovr.exe patches counted below (the provider-name redirect and Patch 3) — named so the
    // "init complete" summary can show a baseline instead of a bare count.
    constexpr int kEchovrPatchCount = 2;
    int patched = 0;

    /* Provider name: "pnsovr" and "pnsdemo" load "pnsrad" (provider_name_redirect.h). A server that cannot
     * install it would load the stock module, so ServerFatal refuses to start it; a client logs the Warning
     * and continues. */
    if (nevr_provider_name_redirect::Install(base_addr)) {
        patched++;
    } else {
        ServerFatal("pnsrad provider name redirect (CNSProvider::LoadByName) not installed: pnsovr and pnsdemo "
                    "would load the stock social module");
    }

    /* Patch 3: OVR platform branch — NOP the 6-byte JNE */
    {
        auto* p = reinterpret_cast<uint8_t*>(base_addr + OVR_BRANCH);
        if (p[0] == 0x90 && p[1] == 0x90) {
            Log(EchoVR::LogLevel::Debug, "[NEVR.PATCH] pnsrad OVR branch already NOPed");
        } else if (nevr::ValidatePrologue(p, OVR_JNE_EXPECTED, sizeof(OVR_JNE_EXPECTED))) {
            uint8_t nops[] = {0x90, 0x90, 0x90, 0x90, 0x90, 0x90};
            DWORD err = 0;
            if (ProcessMemcpy(p, nops, sizeof(nops), &err)) {
                Log(EchoVR::LogLevel::Info,
                    "[NEVR.PATCH] pnsrad patched OVR branch rva=0x%x (bypasses OVR platform branch)",
                    (unsigned)OVR_BRANCH);
                patched++;
            } else {
                Log(EchoVR::LogLevel::Warning,
                    "[NEVR.PATCH] pnsrad PatchMemory FAILED for OVR branch rva=0x%x error=%lu",
                    (unsigned)OVR_BRANCH, err);
            }
        } else {
            uint8_t actual[6];
            std::memcpy(actual, p, sizeof(actual));
            Log(EchoVR::LogLevel::Warning,
                "[NEVR.PATCH] pnsrad unexpected bytes at OVR branch rva=0x%x expected=%s actual=%s",
                (unsigned)OVR_BRANCH, BytesToHex(OVR_JNE_EXPECTED, sizeof(OVR_JNE_EXPECTED)).c_str(),
                BytesToHex(actual, sizeof(actual)).c_str());
        }
    }

    /* Patch 4 (deferred): record pnsrad.dll and pnsradmatchmaking.dll loads, patch them when LoadLibrary returns */
    {
        nevr_dll_load_hook::AddPostLoadObserver(&ApplyPendingLoadsAtLibraryReturn);
        HMODULE ntdll = GetModuleHandleA("ntdll");
        auto regFn = reinterpret_cast<LdrRegisterDllNotification_fn>(
            GetProcAddress(ntdll, "LdrRegisterDllNotification"));
        if (regFn) {
            NTSTATUS status = regFn(0, OnDllLoaded, nullptr, &s_dllNotifCookie);
            if (status == 0) {
                Log(EchoVR::LogLevel::Debug,
                    "[NEVR.PATCH] pnsrad registered DLL notification for pnsrad.dll patches");
            } else {
                // Same catastrophic failure mode as the missing-export case
                // below: pnsrad.dll never gets patched, so the entire social/
                // login layer (friends/party/login) stays broken for the whole
                // session with no other signal.
                Log(EchoVR::LogLevel::Error,
                    "[NEVR.PATCH] pnsrad LdrRegisterDllNotification failed status=0x%lx — pnsrad.dll "
                    "will never be patched (social/login layer broken)", (unsigned long)status);
            }
        } else {
            Log(EchoVR::LogLevel::Error,
                "[NEVR.PATCH] pnsrad LdrRegisterDllNotification not found in ntdll — pnsrad.dll will "
                "NEVER be patched (social/login layer broken)");
        }
    }

    Log(EchoVR::LogLevel::Info, "[NEVR.PATCH] pnsrad init complete: %d/%d echovr.exe patches applied",
        patched, kEchovrPatchCount);
#else
    (void)base_addr;
#endif
}

void nevr_pnsrad_enabler::Shutdown() {
#ifdef _WIN32
    if (s_dllNotifCookie) {
        HMODULE ntdll = GetModuleHandleA("ntdll");
        auto unregFn = reinterpret_cast<LdrUnregisterDllNotification_fn>(
            GetProcAddress(ntdll, "LdrUnregisterDllNotification"));
        if (unregFn) unregFn(s_dllNotifCookie);
        s_dllNotifCookie = nullptr;
    }
#endif
}

void nevr_pnsrad_enabler::ApplyLoadsMissedAtLibraryReturn() {
#ifdef _WIN32
    ApplyPendingLoads(ApplyPoint::FrameTick);
#endif
}

void nevr_pnsrad_enabler::InstallPendingDiagnostics() {
#ifdef _WIN32
    s_pendingDiagnostics.Drain([](uintptr_t base) {
        const HMODULE loaded = GetModuleHandleA("pnsrad.dll");
        if (reinterpret_cast<uintptr_t>(loaded) != base) {
            Log(EchoVR::LogLevel::Warning,
                "[NEVR.PATCH] pnsrad diagnostic hooks skipped: module changed before deferred install");
            return;
        }
        InstallPartyBroadcasterDiag(base);
        InstallPartySendInviteDiag(base);
    });
#endif
}

uintptr_t nevr_pnsrad_enabler::GetModuleBase() {
    return s_pnsradModuleBase.load(std::memory_order_acquire);
}
