/* GOT targets and callback types for the pinned Quest artifact (docs/adr/0003).
 *
 * Every address here is a link-time address in the pinned ELF, checked against
 * its own relocation table (`readelf -rW`) and, for the typed callbacks, against
 * the Itanium mangling of the symbol the relocation names. They apply only to
 * the build IDs below; GotHook refuses a module whose build ID differs.
 *
 * This header includes callback_thunk.h, so a translation unit that includes it must be
 * built with -fno-exceptions (see the contract there). Handlers are `noexcept`.
 *
 * Nothing in this header activates a hook. entry.cpp installs the clock_gettime
 * proof hook and the login-prompt hooks (login_prompt_hook.h, on
 * SetDelimitedErrorMessage and CR15NetGame::Update); the CJson::TString thunks are
 * declared for the config-string seam and stay uninstalled until the gates in #158 pass.
 */
#pragma once

#include <time.h>

#include <cstddef>
#include <cstdint>

#include "callback_thunk.h"
#include "got_hook.h"

namespace sentinel::pinned {

inline constexpr const char* kLibR15 = "libr15.so";
inline constexpr const char* kLibR15BuildId = "b243509c08ce677aeb95fa348016949b3fc45230";
inline constexpr const char* kMatchmaking = "libpnsradmatchmaking.so";
inline constexpr const char* kMatchmakingBuildId = "8c4fddc079eae65909530132a56c48da48b2708c";

inline constexpr const char* kTStringSymbol = "_ZNK10NRadEngine5CJson7TStringEPKcS2_j";
inline constexpr const char* kClockGettimeSymbol = "clock_gettime";
inline constexpr const char* kConfigRequestSendSymbol =
    "_ZN10NRadEngine18SNSConfigRequestv24SendERNS_15CTcpBroadcasterEPKcS4_";
inline constexpr const char* kSetDelimitedErrorMessageSymbol =
    "_ZN10NRadEngine8NRadGame11CR15NetGame24SetDelimitedErrorMessageEPKc";
inline constexpr const char* kNetGameUpdateSymbol = "_ZN10NRadEngine8NRadGame11CR15NetGame6UpdateEy";
inline constexpr const char* kMountObbSymbol = "AStorageManager_mountObb";
inline constexpr const char* kGetMountedObbPathSymbol = "AStorageManager_getMountedObbPath";
inline constexpr const char* kEnablePageNodeEnterSymbol =
    "_ZN10NRadEngine8NRadGame25CR15UIPage2EnablePageNode5EnterERKNS0_29SR15UIPage2EnablePageNodeDataE";

// ---- typed callbacks --------------------------------------------------------

// libc: int clock_gettime(clockid_t, struct timespec*).
using ClockGettimeSig = int(clockid_t, struct timespec*);
struct ClockGettimeTag {};
using ClockGettimeThunk = CallbackThunk<ClockGettimeTag, ClockGettimeSig>;

// NRadEngine::CJson::TString(char const*, char const*, unsigned int) const, the
// same function in both libraries (const member: `this` in x0, key x1, fallback
// x2, flag w3; returns a borrowed char const* in x0). CJson is never
// dereferenced by a handler, so it stays an incomplete type.
struct CJsonOpaque;
using CJsonTStringSig =
    const char*(const CJsonOpaque* self, const char* key, const char* fallback, std::uint32_t flag);
struct LibR15TStringTag {};
struct MatchmakingTStringTag {};
using LibR15TStringThunk = CallbackThunk<LibR15TStringTag, CJsonTStringSig>;
using MatchmakingTStringThunk = CallbackThunk<MatchmakingTStringTag, CJsonTStringSig>;

// NRadEngine::NRadGame::CR15NetGame::SetDelimitedErrorMessage(char const*), defined in libr15 at
// 0x125f768 (member: `this` in x0, the message in x1, no return value). It splits the message on
// '\n' (CStringTable(msg, 10)) into at most four lines and stores them with
// CR15NetGame::SetErrorMessage, which copies each line into the error block (game_layout below)
// and logs them as "[NETGAME] %s %s %s %s". Its three callers are the login error callbacks,
// each followed by SwitchTo(-0x5e): LogInFailedCB (bl at 0x125f604), LoginRemovedCB (0x125f9b4,
// only when the state is >= 3, logged in) and LocalUserProfileErrorCB (0x126d588, only when the
// state is 2, logging in). The message is borrowed for the call only.
struct CR15NetGameOpaque;
using SetDelimitedErrorMessageSig = void(CR15NetGameOpaque* self, const char* message);
struct LibR15SetDelimitedErrorMessageTag {};
using LibR15SetDelimitedErrorMessageThunk =
    CallbackThunk<LibR15SetDelimitedErrorMessageTag, SetDelimitedErrorMessageSig>;

// Android NDK <android/storage_manager.h>, imported by libr15.so (and by the static copies of the engine in
// libpnsovr/libpnsrad/libpnsradmatchmaking, which are not hooked). CSysFile::Init (0xf85260) calls
// AStorageManager_mountObb(mgr, "<obb path>", "37c70a1635a1ad7a", CSysFile_OnObbStateChange, nullptr) at
// 0xf85434, then polls the flag byte at 0x37623c0 that the callback sets, for about 30 s. Horizon OS refuses
// keyed OBB mounts ("mounting encrypted OBBs is no longer supported") and never calls the callback.
//   void AStorageManager_mountObb(AStorageManager*, const char* filename, const char* key,
//                                 AStorageManager_obbCallbackFunc cb, void* data);
//   typedef void (*AStorageManager_obbCallbackFunc)(const char* filename, const int32_t state, void* data);
//   const char* AStorageManager_getMountedObbPath(AStorageManager*, const char* filename);
// CSysFile_OnObbStateChange (0xf86c28) reads only `state` (a jump table over 1..0x19; 1 and 0x18 are the
// success cases). On state 1 it calls AStorageManager_getMountedObbPath with the manager the game stored at
// 0x37623e0 and the path buffer at 0x37623f0, copies the result into gOBBPath (0x3762470), logs
// "OBB mounted at '%s'" and sets the flag byte. A NULL path zero-fills gOBBPath and logs the same line.
// GetDataRootDir (0xf87250) returns gOBBPath when its first byte is non-zero, else
// "/storage/emulated/0/readyatdawn" (0x2b2f5c3).
struct AStorageManagerOpaque;
using ObbCallbackFn = void (*)(const char* filename, std::int32_t state, void* data);
using MountObbSig = void(AStorageManagerOpaque* manager, const char* filename, const char* key, ObbCallbackFn callback,
                         void* data);
struct LibR15MountObbTag {};
using LibR15MountObbThunk = CallbackThunk<LibR15MountObbTag, MountObbSig>;
using GetMountedObbPathSig = const char*(AStorageManagerOpaque* manager, const char* filename);
struct LibR15GetMountedObbPathTag {};
using LibR15GetMountedObbPathThunk = CallbackThunk<LibR15GetMountedObbPathTag, GetMountedObbPathSig>;
// AOBB_STATE_MOUNTED, the success state the game's callback treats as "mounted".
inline constexpr std::int32_t kObbStateMounted = 1;
// The data root the game already falls back to when no OBB path was recorded (GetDataRootDir, above).
inline constexpr const char* kObbFallbackDataRoot = "/storage/emulated/0/readyatdawn";

// NRadEngine::NRadGame::CR15UIPage2EnablePageNode::Enter(SR15UIPage2EnablePageNodeData const&), defined in
// libr15 at 0x1fc210c: the UI script node that enables a page. `node` (x0) is the node object, which is the
// data record plus 0x20 (`node == data + 0x20`, checked by the hook), and `data` (x1) is the record. It
// returns nothing (the one caller, BindBranchingNode at 0x2005aa4, ignores x0), writes nothing to the thread
// or the node, and either enables the page (EnablePage 0x1f79314, directly or deferred) or reports a script
// component error. Skipping the call, as a plain return, leaves the page disabled and the script thread
// where it was. It can run on a task-scheduler worker thread (CScriptCS::UpdateScripts may run instances
// through CComponentSystem::TaskedUpdate), so a handler reads atomics and writes at most one rate-limited
// log line built in a stack buffer.
using EnablePageNodeEnterSig = void(void* node, const void* data);
struct LibR15EnablePageNodeEnterTag {};
using LibR15EnablePageNodeEnterThunk = CallbackThunk<LibR15EnablePageNodeEnterTag, EnablePageNodeEnterSig>;

// SR15UIPage2EnablePageNodeData as the hook reads it: +0x10 is the target page's level actor id (set by
// InitScriptVars 0x168b56c from the script variable binding, kept by RefreshActors 0x25f6824 even when the
// actor is not resolved), +0x20 is the node object. The ids are the pages' symbol hashes in the main menu
// level (f927772b9e2aefb1).
namespace ui_layout {
inline constexpr std::size_t kEnablePageActorIdOffset = 0x10;
inline constexpr std::size_t kEnablePageNodeOffset = 0x20;
inline constexpr std::uint64_t kErrorDisplayPage = 0x4b8a0630361f3ac5ULL;       // error_display_page
inline constexpr std::uint64_t kFatalErrorDisplayPage = 0xe26415a8c369eb2eULL;  // fatal_error_display_page
inline constexpr std::uint64_t kLoggingInPage = 0xee753e35461e0ef4ULL;          // logging_in_page

// Page actor ids seen in ui_page_enter lines, with the name the level's scripts give each. A page's id is a
// level actor id, not CSymbol64 of its name, so the names come from the pinned build's data: each script
// component of the three levels that hold UIPage2 components (resource type f31aed40bf478d4e; levels
// f927772b9e2aefb1 main menu, 05361c73bb73db19, e962a897e2cb8f07) binds script variables to actors in its
// array at record +0x228 ({key, 0xffffffffffffffff, target actor id, 0}); the script library's
// script_set_variable lists, per slot, the CSymbol64 hash of each variable's name followed by the key, and
// every name here is a string in that library's .rodata (or a CSymbol64 hash match for a word the library
// carries only as a constant: empty_page, home_page, error_display_page). Scripts name a page
// relative to themselves, so a few ids have more than one name: the one most bindings use is listed
// (home_page: 8c94450216e31162 by 20 of 23 bindings, 80d0b99e73cf486a by 14 of 17; initial_popups_page 6 of
// 8; empty_select_page 3 of 4; page_quit_confirm 2 of 3). An id no binding names
// (202763036b7f6f23, the boot page) and the no-actor id 0xffffffffffffffff stay "unknown".
struct PageName {
  std::uint64_t id;
  const char* name;
};
inline constexpr PageName kPageNames[] = {
    {0x05ce113632359ce3ULL, "home_page"},
    {0x1a92c34885065c11ULL, "empty_page"},
    {0x1f5bccac7f496eddULL, "loading_page"},
    {0x25cbdc13a0ccdf42ULL, "empty_select_page"},
    {0x2fe7102931d1c4e0ULL, "begin_multiplayer_page"},
    {kErrorDisplayPage, "error_display_page"},
    {0x68db70ece7c24901ULL, "home_page"},
    {0x80d0b99e73cf486aULL, "home_page"},
    {0x8c94450216e31162ULL, "home_page"},
    {0x9143e219cb923869ULL, "store_empty_intermediate_page"},
    {0x9733f27f738d9595ULL, "home_page"},
    {0xb245345073f0d3b3ULL, "connecting_page"},
    {0xc615ef51fe8c7e5bULL, "initial_popups_page"},
    {0xd436ecc9f7f9164dULL, "page_quit_confirm"},
    {0xdadda9a8c9c49f8dULL, "group_popups_page"},
    {kFatalErrorDisplayPage, "fatal_error_display_page"},
    {kLoggingInPage, "logging_in_page"},
    {0xfbc6a43d068f418dULL, "transition_to_game_page"},
};
}  // namespace ui_layout

// NRadEngine::NRadGame::CR15NetGame::Update(unsigned long long), defined in libr15 at 0x1294b40,
// called from CR15Game::UpdateGame (bl at 0x11fb5cc, the only call site, up to four times per
// game-loop iteration with UpdateGame's own argument;
// the caller does not read x0 afterwards: the next instruction loads x0 for CncaGame::UpdateGame).
using CR15NetGameUpdateSig = void(CR15NetGameOpaque* self, std::uint64_t arg);
struct LibR15NetGameUpdateTag {};
using LibR15NetGameUpdateThunk = CallbackThunk<LibR15NetGameUpdateTag, CR15NetGameUpdateSig>;

// NRadEngine::NRadGame::CR15NetGame::QuitOnError(), defined in libr15 at 0x12713f8 (member: `this` in
// x0, no other argument, no return value; exported, so it is also in .dynsym). It is the game's own
// error event: with the game space present (CR15Game+0x7af0) it sends the component event whose
// handler makes the UI status script copy the error block into its text elements again. The login
// prompt hook calls it when it has rewritten the block while the game sits in "login failed", so a new
// code reaches the screen. It is not hooked and has no GOT slot of its own to pin (its only slot is the
// GLOB_DAT of an InvokeExclusiveUpdate instantiation), so it is reached the way the install proves it:
// the module's build ID, then the function's first four instructions, then base + this address.
inline constexpr const char* kQuitOnErrorSymbol = "_ZN10NRadEngine8NRadGame11CR15NetGame11QuitOnErrorEv";
inline constexpr std::uint64_t kQuitOnErrorVaddr = 0x12713f8;
// stp x19, x30, [sp, #-16]!; mov x19, x0; ldr x0, [x0, #8]; ldr x8, [x19, #0x2da0].
inline constexpr std::uint32_t kQuitOnErrorCode[4] = {0xa9bf7bf3, 0xaa0003f3, 0xf9400400, 0xf956d268};
using QuitOnErrorSig = void(CR15NetGameOpaque* self);

// CR15NetGame fields the login-prompt hook reads and writes, measured on the pinned libr15:
namespace game_layout {
// EState at offset 0: SwitchTo (0x125b8b4) compares `ldr w0, [x0]` with the new state and stores
// it with `str w20, [x19]` (0x125b9f4). Names from GameStateString (0x124d478).
inline constexpr std::size_t kStateOffset = 0;
inline constexpr std::int32_t kStateLoggingIn = 2;      // "logging in"
inline constexpr std::int32_t kStateLoginFailed = -94;  // "login failed"
inline constexpr std::int32_t kStateLoadingGlobal = 4;  // "loading global" (GameStateString 0x124d478)
// The error block SetErrorMessage(4 args) (0x1241430) writes: a byte at +0x63308 (1 for the 2- and
// 4-line forms, 0 for the 1-line form), then four 64-byte lines from +0x63309, each forced to end
// in NUL at its 64th byte; CR15NetErrorMessageExpression::operator() (0x23225d0) copies these
// 0x101 bytes to the UI script.
inline constexpr std::size_t kErrorBlockOffset = 0x63308;
inline constexpr std::size_t kErrorLineBytes = 0x40;
inline constexpr std::size_t kErrorLines = 4;
inline constexpr std::size_t kErrorBlockBytes = 1 + kErrorLines * kErrorLineBytes;
}  // namespace game_layout

// SNSConfigRequestv24Send(CTcpBroadcaster&, char const*, char const*): the
// mangling encodes the parameters but not the return type, and no caller has
// been decoded, so there is deliberately no thunk for it.

// ---- targets ----------------------------------------------------------------

// libr15.so imports clock_gettime from libc (JUMP_SLOT 0x36c33e8).
inline GotTarget LibR15ClockGettime() {
  return {kLibR15, kClockGettimeSymbol, RelocKind::kJumpSlot, kLibR15BuildId, 0x36c33e8ULL};
}

// libr15.so's slot for CJson::TString, defined in libr15 itself (JUMP_SLOT 0x36ebe08).
inline GotTarget LibR15TString() {
  return {kLibR15, kTStringSymbol, RelocKind::kJumpSlot, kLibR15BuildId, 0x36ebe08ULL};
}

// libr15.so's slot for CR15NetGame::SetDelimitedErrorMessage, defined in libr15 itself
// (PLT 0xf23510 loads JUMP_SLOT 0x36e9170).
inline GotTarget LibR15SetDelimitedErrorMessage() {
  return {kLibR15, kSetDelimitedErrorMessageSymbol, RelocKind::kJumpSlot, kLibR15BuildId, 0x36e9170ULL};
}

// libr15.so's slot for CR15NetGame::Update, defined in libr15 itself (PLT 0xf11e20 loads
// JUMP_SLOT 0x36e05f8).
inline GotTarget LibR15NetGameUpdate() {
  return {kLibR15, kNetGameUpdateSymbol, RelocKind::kJumpSlot, kLibR15BuildId, 0x36e05f8ULL};
}

// libr15.so's slot for CR15UIPage2EnablePageNode::Enter, defined in libr15 itself (JUMP_SLOT 0x36c1cf8).
inline GotTarget LibR15EnablePageNodeEnter() {
  return {kLibR15, kEnablePageNodeEnterSymbol, RelocKind::kJumpSlot, kLibR15BuildId, 0x36c1cf8ULL};
}

// libr15.so's slots for the two NDK storage-manager imports (JUMP_SLOT 0x36c39d0 and 0x36f5b48, readelf -rW
// on the pinned image; both are imported, value 0).
inline GotTarget LibR15MountObb() {
  return {kLibR15, kMountObbSymbol, RelocKind::kJumpSlot, kLibR15BuildId, 0x36c39d0ULL};
}
inline GotTarget LibR15GetMountedObbPath() {
  return {kLibR15, kGetMountedObbPathSymbol, RelocKind::kJumpSlot, kLibR15BuildId, 0x36f5b48ULL};
}

// libpnsradmatchmaking.so's slot for the same function, defined in
// libpnsradmatchmaking itself at 0x209484 (JUMP_SLOT 0x6b4768).
inline GotTarget MatchmakingTString() {
  return {kMatchmaking, kTStringSymbol, RelocKind::kJumpSlot, kMatchmakingBuildId, 0x6b4768ULL};
}

// libr15.so's slot for SNSConfigRequestv24Send, a weak function it defines at
// 0x191625c (JUMP_SLOT 0x37015e0).
inline GotTarget LibR15ConfigRequestSend() {
  return {kLibR15, kConfigRequestSendSymbol, RelocKind::kJumpSlot, kLibR15BuildId, 0x37015e0ULL};
}

// Two GLOB_DAT slots in libr15.so (0x3715620 and 0x372b698), used as fixtures
// for the GLOB_DAT path.
inline GotTarget LibR15BindNodeGlobDat() {
  return {kLibR15,
          "_ZN10NRadEngine8BindNodeINS_8NRadGame28SR15NetConfigRequestNodeDataENS1_24CR15NetConfigRequestNodeEEExRNS_13SScriptThreadEPv",
          RelocKind::kGlobDat, kLibR15BuildId, 0x3715620ULL};
}
inline GotTarget LibR15LookupDataBindingGlobDat() {
  return {kLibR15,
          "_ZN10NRadEngine17LookupDataBindingINS_8NRadGame28SR15NetConfigRequestNodeDataEEEvNS_9CSymbol64EPKciPiPPFvPviS7_E",
          RelocKind::kGlobDat, kLibR15BuildId, 0x372b698ULL};
}

}  // namespace sentinel::pinned
