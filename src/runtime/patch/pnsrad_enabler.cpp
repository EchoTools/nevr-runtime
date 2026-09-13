/* SYNTHESIS -- custom tool code, not from binary */

/* ======================================================================
 * pnsrad_enabler — Force pnsrad.dll as the social platform
 *
 * EchoVR selects between social platform DLLs ("pnsovr", "pnsdemo", "pnsrad")
 * via flag-driven logic. This module:
 *
 *   1. Overwrites "pnsovr" and "pnsdemo" string data so all code paths load
 *      pnsrad.dll regardless of flags.
 *
 *   2. NOPs the OVR platform branch so PlatformModuleDecisionAndInitialize
 *      takes Path 2 (full networking).
 *
 *   3. Registers an LdrDllNotification callback to patch pnsrad.dll's login
 *      provider check the moment it loads.
 *
 *   4. NOPs LogInSuccessCB's identity comparison so it processes LoginSuccess
 *      even with uninitialized local identity.
 *
 *   5. NOPs LoginIdResponseCB's authenticated state flag check so GameSettings
 *      are processed even when the injected LoginRequest bypasses pnsrad's
 *      state machine.
 * ====================================================================== */

#include "runtime/patch/pnsrad_enabler.h"
#include "core/logging.h"
#include "nevr_common.h"      // N97: the one ValidatePrologue

#include <cstring>

#ifdef _WIN32
#include <windows.h>
#include <winternl.h>
#include <MinHook.h>
#endif

#ifdef _WIN32

/* --------------------------------------------------------------------
 * Patch addresses (RVA from image base)
 * -------------------------------------------------------------------- */

// echovr.exe string data
static constexpr uintptr_t STR_PNSOVR  = 0x16d35c4;
static constexpr uintptr_t STR_PNSDEMO = 0x16d35e8;
static constexpr size_t    STR_SIZE    = 7;  // "pnsrad\0"

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
// ws_bridge.cpp already binds a second listener on port 42148 for exactly
// this (N146 comment there), but nothing connected to it because the game
// never tried — it was stuck on the compiled default.
//
// Load timing, confirmed live via a strace-style empirical test (renamed
// pnsradmatchmaking.dll aside, launched, watched the client log): it loads
// on demand at the lobby stage, well after login —
// "[EVR] [NSLOBBY] loading matchmaking library 'pnsradmatchmaking'" — via a
// native LoadLibrary-style path in cnslobby.cpp (confirmed by the clean
// "failed to load module ... Unable to load matchmaking library" error when
// the file was absent). LdrRegisterDllNotification catches this regardless
// of when it fires, so no ordering change was needed against the existing
// pnsrad.dll callback below.
//
// pnsradmatchmaking.dll, ImageBase 0x180000000 (confirmed via
// `objdump -p pnsradmatchmaking.dll | grep -i imagebase`):
//   RVA 0x1c84d8 (.rdata section, VMA 0x1801c6000, file offset 0x1c5200 per
//   `objdump -h`; string file offset = 0x1c76d8, verified byte-for-byte via
//   `dd if=pnsradmatchmaking.dll bs=1 skip=$((0x1c76d8)) count=64 | xxd`) —
//   a 49-byte slot (the 48-char string + NUL, then unrelated data
//   immediately follows with no padding) holding
//   "wss://matchmaker.readyatdawn.com/rad/rad15_live\0".
// Replacing with "ws://127.0.0.1:42148\0" (21 bytes) leaves the tail of the
// original slot as inert bytes after our new NUL terminator — same pattern
// xpid_patch.cpp already uses for a shorter replacement in a fixed slot.
//
// CONFIRMED LIVE 2026-09-13: patch applied ("[pnsradmatchmaking] patched
// matchmaker host default at +0x1c84d8"), matchmaker connected through our
// own listener, "[NSLOBBY] received lobby session success", joined a real
// server (108.218.163.196:6792), loaded into a live social lobby.
static constexpr uintptr_t PNSRADMATCHMAKING_HOST_RVA = 0x1c84d8;
static constexpr size_t    PNSRADMATCHMAKING_HOST_SLOT_SIZE = 49;
static constexpr char      PNSRADMATCHMAKING_HOST_EXPECTED[] =
    "wss://matchmaker.readyatdawn.com/rad/rad15_live";
static constexpr char      PNSRADMATCHMAKING_HOST_REPLACEMENT[] = "ws://127.0.0.1:42148";

// 2026-09-13 (Andrew/ReVault audit): CNSRADParty (and, per the same audit,
// CNSRADFriends/CNSRADUsers/CNSRADActivities) register every inbound SNS
// listener via CTcpBroadcaster::Listen using a broadcaster HANDLE stored in
// the object itself — *(this+0x2b0) for CNSRADParty. If that handle is null
// at the moment this runs, every Listen() call it makes (InviteNotifyCB
// included) silently registers nothing — no error, no log — which would
// fully explain why a live party-invite test produced zero evidence
// anywhere (see ReVault comments on 0x3039c4/libpnsrad.so and
// 0x180082d20/pnsrad.dll). This diagnostic-only hook confirms or falsifies
// that live, rather than continuing to infer from static analysis.
//
// Target: the vslot[8] function that does the Listen() registration loop
// (pnsrad.dll FUN_180082d20, confirmed via ReVault to read the handle at
// param_1[0x56] == *(this+0x2b0) before every Listen call). RVA computed
// against pnsrad.dll's 0x180000000 image base (same convention used
// throughout this file for pnsradmatchmaking.dll's RVA above).
static constexpr uintptr_t PNSRAD_PARTY_INITIALIZE_RVA = 0x82d20;
static constexpr uintptr_t PNSRAD_PARTY_BROADCASTER_HANDLE_OFFSET = 0x2b0;

/* --------------------------------------------------------------------
 * Memory patching
 * -------------------------------------------------------------------- */

static bool PatchMemory(void* addr, const void* data, size_t len) {
    DWORD oldProtect;
    if (!VirtualProtect(addr, len, PAGE_EXECUTE_READWRITE, &oldProtect)) return false;
    std::memcpy(addr, data, len);
    VirtualProtect(addr, len, oldProtect, &oldProtect);
    return true;
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
static bool  s_pnsradPatched  = false;

/* Case-insensitive ASCII wide-string compare, same folding convention as
 * initialize.cpp's LoadNameContains. UNICODE_STRING::Length is bytes, not
 * chars, hence the /sizeof(WCHAR) below at each call site. */
static bool WideNameEqualsAscii(const WCHAR* name, size_t nameLen, const char* ascii) {
    size_t asciiLen = std::strlen(ascii);
    if (nameLen != asciiLen) return false;
    for (size_t i = 0; i < asciiLen; i++) {
        WCHAR c = name[i];
        if (c >= L'A' && c <= L'Z') c = static_cast<WCHAR>(c - L'A' + L'a');
        char e = ascii[i];
        if (e >= 'A' && e <= 'Z') e = static_cast<char>(e - 'A' + 'a');
        if (c != static_cast<WCHAR>(e)) return false;
    }
    return true;
}

/* Patch pnsradmatchmaking.dll's compiled matchmaker-host default so the
 * matchmaker connection reaches our own listener instead of the dead
 * readyatdawn.com host. See PNSRADMATCHMAKING_HOST_RVA's comment above for
 * the full measurement (RVA, file offset, slot size, exact original bytes). */
static void PatchMatchmakingHost(uintptr_t base) {
    auto* site = reinterpret_cast<uint8_t*>(base + PNSRADMATCHMAKING_HOST_RVA);
    if (std::memcmp(site, PNSRADMATCHMAKING_HOST_EXPECTED,
                     sizeof(PNSRADMATCHMAKING_HOST_EXPECTED) - 1) != 0) {
        Log(EchoVR::LogLevel::Warning,
            "[pnsradmatchmaking] unexpected bytes at +0x%x — NOT patched (measured "
            "against a different build?)", (unsigned)PNSRADMATCHMAKING_HOST_RVA);
        return;
    }
    // Replacement is shorter than the original slot (21 of 49 bytes); the
    // trailing original bytes become inert garbage after our new NUL, same
    // as xpid_patch.cpp's shorter-replacement-in-a-fixed-slot pattern.
    if (PatchMemory(site, PNSRADMATCHMAKING_HOST_REPLACEMENT,
                     sizeof(PNSRADMATCHMAKING_HOST_REPLACEMENT))) {
        Log(EchoVR::LogLevel::Info,
            "[pnsradmatchmaking] patched matchmaker host default at +0x%x: "
            "\"%s\" -> \"%s\"", (unsigned)PNSRADMATCHMAKING_HOST_RVA,
            PNSRADMATCHMAKING_HOST_EXPECTED, PNSRADMATCHMAKING_HOST_REPLACEMENT);
    } else {
        Log(EchoVR::LogLevel::Warning,
            "[pnsradmatchmaking] PatchMemory FAILED at +0x%x — prologue matched "
            "but the write did not land", (unsigned)PNSRADMATCHMAKING_HOST_RVA);
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
    Log(EchoVR::LogLevel::Info,
        "[pnsrad] DIAG CNSRADParty listener-register: this=%p broadcaster_handle=%p (%s)",
        thisPtr, reinterpret_cast<void*>(handle), handle == 0 ? "NULL" : "non-null");
    return g_RealPartyListenerRegister(thisPtr, param2);
}

static void InstallPartyBroadcasterDiag(uintptr_t base) {
    void* target = reinterpret_cast<void*>(base + PNSRAD_PARTY_INITIALIZE_RVA);
    MH_STATUS st = MH_CreateHook(target, reinterpret_cast<void*>(PartyListenerRegisterHook),
                                  reinterpret_cast<void**>(&g_RealPartyListenerRegister));
    if (st == MH_OK) st = MH_EnableHook(target);
    if (st == MH_OK) {
        Log(EchoVR::LogLevel::Info,
            "[pnsrad] DIAG party-broadcaster hook installed at +0x%x",
            (unsigned)PNSRAD_PARTY_INITIALIZE_RVA);
    } else {
        Log(EchoVR::LogLevel::Warning,
            "[pnsrad] DIAG party-broadcaster hook FAILED at +0x%x: %s",
            (unsigned)PNSRAD_PARTY_INITIALIZE_RVA, MH_StatusToString(st));
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
 * previously-silent PatchMemory failure. */
static void PnsradNopPatch(uint8_t* site, const uint8_t* expected, size_t expLen,
                           size_t nopLen, const char* what, unsigned rva) {
    if (!nevr::ValidatePrologue(site, expected, expLen)) {
        Log(EchoVR::LogLevel::Warning,
            "[pnsrad] unexpected bytes at %s +0x%x — NOT patched", what, rva);
        s_pnsradFail++;
        return;
    }
    uint8_t nops[8];
    for (size_t i = 0; i < nopLen && i < sizeof(nops); i++) nops[i] = 0x90;
    if (PatchMemory(site, nops, nopLen)) {
        Log(EchoVR::LogLevel::Debug, "[pnsrad] patched %s at +0x%x", what, rva);
        s_pnsradOk++;
    } else {
        Log(EchoVR::LogLevel::Warning,
            "[pnsrad] PatchMemory FAILED at %s +0x%x — prologue matched but the write "
            "did not land", what, rva);
        s_pnsradFail++;
    }
}

static void CALLBACK OnDllLoaded(ULONG reason, const LDR_DLL_NOTIFICATION_DATA* data, void*) {
    if (reason != 1 || !data || !data->BaseDllName) return;
    const UNICODE_STRING* name = data->BaseDllName;

    // pnsradmatchmaking.dll: independent of the pnsrad.dll check below — it
    // loads much later (native CNSLobby module load, well after login, per
    // the r14 log's "loading matchmaking library 'pnsradmatchmaking'") and
    // must not be gated on s_pnsradPatched.
    //
    // 2026-09-13 (BUGS.md 81c7e6b): this DLL unloads/reloads mid-session — a
    // reload gets a fresh DllBase with the original (unpatched) bytes, so a
    // one-shot guard here (as pnsrad.dll's s_pnsradPatched below correctly
    // uses, since that DLL doesn't reload) left the reloaded copy unpatched
    // and the matchmaker fell back to the dead readyatdawn.com default —
    // blank terminal, no queue. No guard: patch unconditionally on every
    // load. Idempotent by construction — PatchMatchmakingHost's own memcmp
    // against PNSRADMATCHMAKING_HOST_EXPECTED no-ops (with a Warning log)
    // if this exact base was somehow already patched.
    if (WideNameEqualsAscii(name->Buffer, name->Length / sizeof(WCHAR),
                             "pnsradmatchmaking.dll")) {
        PatchMatchmakingHost(reinterpret_cast<uintptr_t>(data->DllBase));
    }

    if (s_pnsradPatched) return;
    if (name->Length < 10 * sizeof(WCHAR)) return;

    const WCHAR* p = name->Buffer;
    if ((p[0] == L'p' || p[0] == L'P') &&
        (p[1] == L'n' || p[1] == L'N') &&
        (p[2] == L's' || p[2] == L'S') &&
        (p[3] == L'r' || p[3] == L'R') &&
        (p[4] == L'a' || p[4] == L'A') &&
        (p[5] == L'd' || p[5] == L'D') &&
        p[6] == L'.' &&
        (p[7] == L'd' || p[7] == L'D') &&
        (p[8] == L'l' || p[8] == L'L') &&
        (p[9] == L'l' || p[9] == L'L')) {

        s_pnsradPatched = true;
        uintptr_t base = reinterpret_cast<uintptr_t>(data->DllBase);

        PnsradNopPatch(reinterpret_cast<uint8_t*>(base + PNSRAD_LOGIN_CHECK),
                       PNSRAD_JNE_EXPECTED, sizeof(PNSRAD_JNE_EXPECTED), 2,
                       "login check", (unsigned)PNSRAD_LOGIN_CHECK);
        PnsradNopPatch(reinterpret_cast<uint8_t*>(base + PNSRAD_LOGIN_IDENTITY_CHECK),
                       PNSRAD_IDENTITY_JNE_EXPECTED, sizeof(PNSRAD_IDENTITY_JNE_EXPECTED), 6,
                       "identity guard", (unsigned)PNSRAD_LOGIN_IDENTITY_CHECK);
        PnsradNopPatch(reinterpret_cast<uint8_t*>(base + PNSRAD_LOGIN_STATE_CHECK),
                       PNSRAD_STATE_JE_EXPECTED, sizeof(PNSRAD_STATE_JE_EXPECTED), 6,
                       "state check", (unsigned)PNSRAD_LOGIN_STATE_CHECK);
        InstallPartyBroadcasterDiag(base);

        Log(EchoVR::LogLevel::Info,
            "[pnsrad] module patches: %d succeeded, %d failed — social layer "
            "(friends/party/login) %s",
            s_pnsradOk, s_pnsradFail,
            (s_pnsradFail == 0 && s_pnsradOk == 3) ? "ENABLED"
            : (s_pnsradOk == 0) ? "NOT PATCHED"
                                : "PARTIALLY PATCHED");
    }
}

#endif // _WIN32

/* ====================================================================
 * Public API
 * ==================================================================== */

void PnsradEnabler::Init(uintptr_t base_addr) {
#ifdef _WIN32
    int patched = 0;

    /* Patch 1: "pnsovr" -> "pnsrad" */
    {
        auto* p = reinterpret_cast<uint8_t*>(base_addr + STR_PNSOVR);
        if (std::memcmp(p, "pnsovr", 6) == 0) {
            if (PatchMemory(p, "pnsrad\0", STR_SIZE)) {
                Log(EchoVR::LogLevel::Info, "[pnsrad] patched \"pnsovr\" -> \"pnsrad\"");
                patched++;
            }
        } else if (std::memcmp(p, "pnsrad", 6) == 0) {
            Log(EchoVR::LogLevel::Debug, "[pnsrad] pnsovr already \"pnsrad\"");
        }
    }

    /* Patch 2: "pnsdemo" -> "pnsrad" */
    {
        auto* p = reinterpret_cast<uint8_t*>(base_addr + STR_PNSDEMO);
        if (std::memcmp(p, "pnsdemo", 7) == 0) {
            if (PatchMemory(p, "pnsrad\0", STR_SIZE)) {
                Log(EchoVR::LogLevel::Info, "[pnsrad] patched \"pnsdemo\" -> \"pnsrad\"");
                patched++;
            }
        } else if (std::memcmp(p, "pnsrad", 6) == 0) {
            Log(EchoVR::LogLevel::Debug, "[pnsrad] pnsdemo already \"pnsrad\"");
        }
    }

    /* Patch 3: OVR platform branch — NOP the 6-byte JNE */
    {
        auto* p = reinterpret_cast<uint8_t*>(base_addr + OVR_BRANCH);
        if (p[0] == 0x90 && p[1] == 0x90) {
            Log(EchoVR::LogLevel::Debug, "[pnsrad] OVR branch already NOPed");
        } else if (nevr::ValidatePrologue(p, OVR_JNE_EXPECTED, sizeof(OVR_JNE_EXPECTED))) {
            uint8_t nops[] = {0x90, 0x90, 0x90, 0x90, 0x90, 0x90};
            if (PatchMemory(p, nops, sizeof(nops))) {
                Log(EchoVR::LogLevel::Debug, "[pnsrad] patched OVR branch at +0x%x", (unsigned)OVR_BRANCH);
                patched++;
            }
        } else {
            Log(EchoVR::LogLevel::Warning, "[pnsrad] unexpected bytes at OVR branch +0x%x", (unsigned)OVR_BRANCH);
        }
    }

    /* Patch 4 (deferred): Register DLL notification for pnsrad.dll patches */
    {
        HMODULE ntdll = GetModuleHandleA("ntdll");
        auto regFn = reinterpret_cast<LdrRegisterDllNotification_fn>(
            GetProcAddress(ntdll, "LdrRegisterDllNotification"));
        if (regFn) {
            NTSTATUS status = regFn(0, OnDllLoaded, nullptr, &s_dllNotifCookie);
            if (status == 0) {
                Log(EchoVR::LogLevel::Debug, "[pnsrad] registered DLL notification for pnsrad.dll patches");
            } else {
                Log(EchoVR::LogLevel::Warning, "[pnsrad] LdrRegisterDllNotification failed: 0x%lx", (unsigned long)status);
            }
        }
    }

    Log(EchoVR::LogLevel::Info, "[pnsrad] init complete (%d echovr.exe patches)", patched);
#else
    (void)base_addr;
#endif
}

void PnsradEnabler::Shutdown() {
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
