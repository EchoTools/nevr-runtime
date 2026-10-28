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
inline constexpr const char* kCreateConnectionSymbol = "_ZN10NRadEngine8CSysHttp16CreateConnectionERmPKc";
inline constexpr const char* kConfigRequestSendSymbol =
    "_ZN10NRadEngine18SNSConfigRequestv24SendERNS_15CTcpBroadcasterEPKcS4_";
inline constexpr const char* kSetDelimitedErrorMessageSymbol =
    "_ZN10NRadEngine8NRadGame11CR15NetGame24SetDelimitedErrorMessageEPKc";
inline constexpr const char* kNetGameUpdateSymbol = "_ZN10NRadEngine8NRadGame11CR15NetGame6UpdateEy";
inline constexpr const char* kMountObbSymbol = "AStorageManager_mountObb";
inline constexpr const char* kGetMountedObbPathSymbol = "AStorageManager_getMountedObbPath";
inline constexpr const char* kAndroidMainSymbol = "android_main";
inline constexpr const char* kFindMethodSymbol = "_ZN10NRadEngine10CSysModule10FindMethodEmPKc";
inline constexpr const char* kGetDeviceExtensionsVulkanSymbol = "vrapi_GetDeviceExtensionsVulkan";
inline constexpr const char* kGetSystemPropertyIntSymbol = "vrapi_GetSystemPropertyInt";
inline constexpr const char* kShowSystemUISymbol = "vrapi_ShowSystemUI";
inline constexpr const char* kSetDisplayRefreshRateSymbol = "vrapi_SetDisplayRefreshRate";
inline constexpr const char* kSetClockLevelsSymbol = "vrapi_SetClockLevels";
inline constexpr const char* kSetPropertyIntSymbol = "vrapi_SetPropertyInt";
inline constexpr const char* kGetSystemPropertyFloatSymbol = "vrapi_GetSystemPropertyFloat";
inline constexpr const char* kSubmitFrame2Symbol = "vrapi_SubmitFrame2";
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

// NRadEngine::CSysHttp::CreateConnection(unsigned long&, char const*), defined in libr15 at 0xf96c08 and
// called through libr15's own PLT (JUMP_SLOT 0x36e8028): `handle` (x0, the `unsigned long&`) receives the
// connection handle and `url` (x1) is the base URL, e.g. the literal "https://api.readyatdawn.com"
// (0x2baa029) that CR15NetStoreTransactions::InitializeHttp (0x126c1bc) passes at 0x126c274, or the
// "https://api-%s.readyatdawn.com" it builds at 0x126c25c (the other literal site is 0x128958c). The
// connection is made synchronously from `url`; the return value (w0, 0 on success: InitializeHttp's
// `cbnz w0` at 0x126c260/0x126c278) is passed through untouched. The URL is borrowed for the call.
using CreateConnectionSig = int(unsigned long* handle, const char* url);
struct LibR15CreateConnectionTag {};
using LibR15CreateConnectionThunk = CallbackThunk<LibR15CreateConnectionTag, CreateConnectionSig>;

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
// NRadEngine::CSysInfo::GetDataRootDir() (libr15 0xf87250, 32 bytes, exported): returns gOBBPath when its first byte is
// non-zero, else the literal above; no argument, the result is a borrowed char const* in x0 that the callers only read.
// By code reading of the pinned image (a disassembly scan; NOT observed on a headset): libr15 calls it through its own
// PLT (stub 0xf34e20, JUMP_SLOT 0x36f1df8) from exactly two sites, the only `bl`/`b` to that stub in the pinned .text: CFileTypes::InitializeDirectories (bl at 0x11eb144, when the root it was
// given is empty) and CEngine::Initialize (bl at 0x2abfe7c, when the engine config carries no data root, the result
// stored at config +0x178). Both take the pointer and copy the string; neither writes through it. No relocation other
// than that JUMP_SLOT names the function (no R_AARCH64_RELATIVE or ABS64 to 0xf87250), so, by that reading, no call
// bypasses the slot. The headset run (#866 Work 4) is what observes it.
inline constexpr const char* kGetDataRootDirSymbol = "_ZN10NRadEngine8CSysInfo14GetDataRootDirEv";
using GetDataRootDirSig = const char*();
struct LibR15GetDataRootDirTag {};
using LibR15GetDataRootDirThunk = CallbackThunk<LibR15GetDataRootDirTag, GetDataRootDirSig>;
// AOBB_STATE_MOUNTED, the success state the game's callback treats as "mounted".
inline constexpr std::int32_t kObbStateMounted = 1;
// The data root the game already falls back to when no OBB path was recorded (GetDataRootDir, above).
inline constexpr const char* kObbFallbackDataRoot = "/storage/emulated/0/readyatdawn";

// NRadEngine::CSysModule::FindMethod(unsigned long module, char const* name), defined in libr15 at 0x2a9e4f8 (72 bytes,
// exported): `module` is the handle CSysModule::Load returned (0 or a null name returns 0), the body calls dlsym through
// libr15's own PLT (0x1016c20, JUMP_SLOT 0x36e2cf8) and returns its result. CR15 Vulkan initialisation
// (Initialize, 0x19dc9d4) reaches vkGetInstanceProcAddr through it: the PLT stub at 0x1008620 loads JUMP_SLOT 0x36db9f8.
using FindMethodSig = void*(unsigned long module, const char* name);
struct LibR15FindMethodTag {};
using LibR15FindMethodThunk = CallbackThunk<LibR15FindMethodTag, FindMethodSig>;

// VrApi_Vulkan.h: ovrResult vrapi_GetDeviceExtensionsVulkan(char* extensionNames, uint32_t* extensionNamesSize).
// libr15 calls it from one wrapper (GetDeviceExtensionsVK 0x1b73dc0: `str w1, [sp, #0xc]; add x1, sp, #0xc; bl <plt>`,
// then `strb wzr, [buf, size]`), passing a 0x800-byte buffer and a pointer to its capacity, ignoring the result.
// Imported (value 0): JUMP_SLOT 0x36dd440.
using VrapiExtensionsSig = std::int32_t(char* extension_names, std::uint32_t* extension_names_size);
struct LibR15GetDeviceExtensionsVulkanTag {};
using LibR15GetDeviceExtensionsVulkanThunk = CallbackThunk<LibR15GetDeviceExtensionsVulkanTag, VrapiExtensionsSig>;

// VrApi.h: int vrapi_GetSystemPropertyInt(const ovrJava* java, ovrSystemProperty propType). Imported (value 0): JUMP_SLOT
// 0x36fa728 (`readelf -rW` on the pinned image), the slot the hardware dump hooks too (docs/adr/0006-quest-hardware-dump.md).
// CVR::RefreshRTSize (0x1a709a0) calls it with ids 5 and 6, the suggested eye texture width and height
// (`bl` at 0x1a709f4 and 0x1a70a04), multiplies the results by CustomViewportScale and stores the pair; CVR::Resize
// (0x1a70d0c) creates the swap chain from it once per process. `java` is never dereferenced here (an incomplete type).
struct OvrJavaOpaque;
using GetSystemPropertyIntSig = int(const OvrJavaOpaque* java, int property);
struct LibR15GetSystemPropertyIntTag {};
using LibR15GetSystemPropertyIntThunk = CallbackThunk<LibR15GetSystemPropertyIntTag, GetSystemPropertyIntSig>;
// NRadEngine::CResourceManager::FillResourceFileRequest(CSymbol64 type, CSymbol64 name, unsigned long long,
// CAllocator*, void*, unsigned long long, SFileRequestData&), defined in libr15 at 0x25439f0 (524 bytes; ReVault lists it
// at 0x26439f0): the single manifest resolver (#871). It clears the request (ClearFileRequestData, 0x2a9759c), binary-searches
// the manifest for (type, name) and on a hit fills the request with the package file handle, the chunk and the words
// that locate the bytes; on a miss it raises "Cannot find resource" and returns the error object. Every reader of a
// manifest resource passes through it before ReadFileAsync: the base loads, the archive loader, the streamed mips and
// the forced full mip load.
//   this (x0, CResourceManager*), type (x1), name (x2), unused by the body (x3), allocator (x4, stored at request+0x460),
//   owner (x5, stored at request+0x00: the CResource* for a base load), index (x6, stored at request+0x08: the buffer
//   index, or the mip index), request (x7, the 0x470-byte SFileRequestData). Returns 0 on a hit.
// CSymbol64 is one u64 passed by value, so a symbol is a plain integer argument here. Called through libr15's own PLT
// stub (0xf30730, JUMP_SLOT 0x36efa80); the thunk passes the game's return address to the handler (kCaller) so a handler
// can tell which of the six call sites it serves.
struct CResourceManagerOpaque;
using FillResourceFileRequestSig = std::uint64_t(CResourceManagerOpaque* manager, std::uint64_t type, std::uint64_t name,
                                                 std::uint64_t unused, void* allocator, void* owner, std::uint64_t index,
                                                 unsigned char* request);
struct LibR15FillResourceFileRequestTag {};
using LibR15FillResourceFileRequestThunk =
    CallbackThunk<LibR15FillResourceFileRequestTag, FillResourceFileRequestSig, /*kCaller=*/true>;
inline constexpr const char* kFillResourceFileRequestSymbol =
    "_ZN10NRadEngine16CResourceManager23FillResourceFileRequestENS_9CSymbol64ES1_yPNS_10CAllocatorEPvyRNS_16SFileRequestDataE";

// SFileRequestData (0x470 bytes), the fields the log-only hook reads after a hit (ClearFileRequestData, 0x2a9759c, sets
// +0x10 and +0x418 to -1; the hit overwrites both). Measured on the pinned libr15 from the function's own stores
// (0x2543a2c-0x2543bf8).
namespace resource_request_layout {
inline constexpr std::size_t kRequestBytes = 0x470;
inline constexpr std::size_t kPackageHandle = 0x10;      // i64: the package file handle, -1 when none
inline constexpr std::size_t kChunkIndex = 0x418;        // i64: the manifest record's chunk index, -1 when none
inline constexpr std::size_t kChunkFileOffset = 0x420;   // u64: the chunk's offset in the package file
inline constexpr std::size_t kChunkCompressed = 0x428;   // u64: the chunk's compressed size
inline constexpr std::size_t kChunkDecompressed = 0x430; // u64: the chunk's decompressed size
inline constexpr std::size_t kOffsetInChunk = 0x438;     // u64: the resource's offset inside the chunk
inline constexpr std::size_t kSize = 0x440;              // u64: the resource's size
inline constexpr std::size_t kAlignment = 0x450;         // u64: the record's alignment
}  // namespace resource_request_layout

// The six call sites of the stub in libr15 (ELF scan of bl to 0xf30730; pinned vaddrs, the address of the bl). The game
// return address a handler sees is the bl plus 4. got_pinned_test.cpp checks each is a bl to the stub in the real library.
inline constexpr std::uint64_t kFillResourceFileRequestStub = 0xf30730;
struct FillCallSite {
  std::uint64_t bl;
  const char* name;
};
inline constexpr FillCallSite kFillCallSites[] = {
    {0x25438f4, "resource_load_a"},    // CResourceManager::ResourceLoadInternal, first buffer
    {0x2543984, "resource_load_b"},    // CResourceManager::ResourceLoadInternal, second buffer
    {0x1341b18, "archive_load_a"},     // the CArchiveLoader request filler (0x1341a88)
    {0x1341b9c, "archive_load_b"},     // the same function, second site
    {0x18e39f0, "mip_request"},        // CGTextureCache::RequestNewMipmaps, per streamed mip
    {0x18e661c, "force_load_all_mips"} // CGTextureResource::BeginForceLoadAllMips
};

// android_main(android_app*), defined in libr15 at 0xf784c4 and called by the native-activity glue's thread entry
// through libr15's own PLT (`mov x0, x19; bl 0xf27bc0` at 0xf79a4c; the stub loads JUMP_SLOT 0x36eb4c8). The glue
// ignores the result and then runs its teardown. android_main shuts the engine down after Main returns and, when
// app->destroyRequested (+0x64) is 0, asks the activity to finish (ANativeActivity_finish, 0xf787b0) and pumps
// the looper until the destroy; it then returns. Nothing on that path ends the process.
struct AndroidAppOpaque;
using AndroidMainSig = void(AndroidAppOpaque* app);
struct LibR15AndroidMainTag {};
using LibR15AndroidMainThunk = CallbackThunk<LibR15AndroidMainTag, AndroidMainSig>;

// NRadEngine::CSysInfo::RequestPermissions(CArrayView<char const*>), defined in libr15 at 0xf873ec (128 bytes) and
// called through libr15's own PLT (0xf19870, JUMP_SLOT 0x36e4320) from CR15Game::Initialize (bl at 0x11f9ce8),
// CRequestPermissionNode::Enter (0x2722814) and BindBranchingNode<..RequestPermission..> (0x2699e28). The array view
// is two words, passed by value in x0 (the element pointer) and x1 (the count), as CR15Game::Initialize loads them
// (0x11f9cb8-0x11f9ce4: three entries, w1 = 3). It logs "Requesting %llu android permissions", builds a
// CJavaStringArray and makes one JNI CallVoidMethod("requestPermissions", "([Ljava/lang/String;I)V") on the
// NativeActivity, then returns. It keeps no state and does not wait for the answer; the boot call in
// CR15Game::Initialize is followed at once by CncaGame::Initialize, whose package-manifest read needs the storage
// permissions (#640). The three names it gets there are RECORD_AUDIO, READ_EXTERNAL_STORAGE, WRITE_EXTERNAL_STORAGE.
using RequestPermissionsSig = void(const char* const* names, std::uint64_t count);
struct LibR15RequestPermissionsTag {};
using LibR15RequestPermissionsThunk = CallbackThunk<LibR15RequestPermissionsTag, RequestPermissionsSig>;

// NRadEngine::CSysInfo::CheckPermission(char const*), defined in libr15 at 0xf87298 (204 bytes, exported): one JNI
// ContextWrapper.checkSelfPermission on the NativeActivity, logs "Checking android permission: '%s' (Result: %d)",
// returns true when the result is 0 (w0 holds 0 or 1). Synchronous; it needs a JNI environment on the calling thread
// (CSysJava::JNI attaches one). Called through libr15's own PLT (JUMP_SLOT 0x36c37c8) by CR15Game::Initialize (bl at
// 0x11f9b98, the RECORD_AUDIO check, the first call in the process), CreateLogFile and the script operators. The
// permission-wait hook has a GOT hook on that slot (LibR15CheckPermissionThunk): the mic check is where it learns that
// the game will NOT call RequestPermissions (CR15Game::Initialize calls it only when this check returns 0). It also
// calls the function itself, the way the install proves it: the module's build ID, then the first four instructions,
// then base + this address (as QuitOnError above).
inline constexpr const char* kCheckPermissionSymbol = "_ZN10NRadEngine8CSysInfo15CheckPermissionEPKc";
inline constexpr std::uint64_t kCheckPermissionVaddr = 0xf87298;
// sub sp, sp, #0x40; str x20, [sp, #0x20]; stp x19, x30, [sp, #0x30]; add x8, sp, #0x10.
inline constexpr std::uint32_t kCheckPermissionCode[4] = {0xd10103ff, 0xf90013f4, 0xa9037bf3, 0x910043e8};
using CheckPermissionSig = bool(const char* permission);
struct LibR15CheckPermissionTag {};
using LibR15CheckPermissionThunk = CallbackThunk<LibR15CheckPermissionTag, CheckPermissionSig>;
inline constexpr const char* kRequestPermissionsSymbol =
    "_ZN10NRadEngine8CSysInfo18RequestPermissionsENS_10CArrayViewIPKcLy0EEE";

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

// Page actor ids seen in ui_page_enter lines, with the name the level scripts give each and how sure that
// name is. A page's id is a level actor id, not CSymbol64 of its name, so the names come from the pinned
// build's data: each script component of the three levels that hold UIPage2 components (resource type
// f31aed40bf478d4e; levels f927772b9e2aefb1 main menu, 05361c73bb73db19, e962a897e2cb8f07) binds script
// variables to actors in its array at record +0x228 ({key, 0xffffffffffffffff, target actor id, 0}), and the
// script library's script_set_variable lists, per slot, the CSymbol64 hash of each variable's name followed
// by the key. Every binding that targets one of these ids is a row of tests/data/ui_page_names.tsv, and
// login_prompt_hook_test derives this table from those rows.
//
// A script names a page relative to itself, so the name is how the scripts that reference the page call it,
// not a name the page carries. name_basis says how the rows support the name: "all" (every named binding
// agrees), "majority" (most agree; the others give a different name), "single" (one named binding). A name
// that several ids would share carries "@" and the first four hex digits of its id. An id whose only name is
// one a parent container's script gives it, while its own script binds other things (05ce113632359ce3, the
// home page content; 68db70ece7c24901, the in-game menu; 9733f27f738d9595, the store, whose own
// script binds store_page, purchase_page and customize_page), and an id no binding names (202763036b7f6f23, the
// boot page; 0xffffffffffffffff, no actor) stay "unknown" with name_basis "none".
struct PageName {
  std::uint64_t id;
  const char* name;
  const char* basis;
};
inline constexpr PageName kPageNames[] = {
    {0x1a92c34885065c11ULL, "empty_page", "all"},
    {0x1f5bccac7f496eddULL, "loading_page", "all"},
    {0x25cbdc13a0ccdf42ULL, "empty_select_page", "majority"},
    {0x2fe7102931d1c4e0ULL, "begin_multiplayer_page", "all"},  // 4 of its 6 bindings are named
    {kErrorDisplayPage, "error_display_page", "all"},
    {0x80d0b99e73cf486aULL, "home_page@80d0", "majority"},
    {0x8c94450216e31162ULL, "home_page@8c94", "majority"},
    {0x9143e219cb923869ULL, "store_empty_intermediate_page", "single"},
    {0xb245345073f0d3b3ULL, "connecting_page", "all"},
    {0xc615ef51fe8c7e5bULL, "initial_popups_page", "majority"},
    {0xd436ecc9f7f9164dULL, "page_quit_confirm", "majority"},
    {0xdadda9a8c9c49f8dULL, "group_popups_page", "all"},
    {kFatalErrorDisplayPage, "fatal_error_display_page", "single"},
    {kLoggingInPage, "logging_in_page", "all"},
    {0xfbc6a43d068f418dULL, "transition_to_game_page", "all"},
};
}  // namespace ui_layout

// CR15VRMenuWidgetCS::Update(), the pointer-ray menu widget's per-frame update, defined in libr15 at pinned 0x199a7a0
// (ReVault 0x209a7a0 = pinned + 0x100000; `revault_function 0x209a7a0`). The engine registers its address
// (R15RegisterComponentSystemUpdates), so no GOT slot names it: the splash skip hooks the entry (inline, arm64).
// Its first four words are `sub sp,sp,#0x110; stp d9,d8; stp x28,x26; stp x25,x24`; the widget fields the hook
// reads (ctx +0x80, defs +0xb8, dense +0xf8, links +0x100, bounds +0xf2/+0xf4, input +0x138) are the ones the
// update itself reads (the PC twin's offsets, checked against this decompilation).
inline constexpr std::uint64_t kWidgetUpdateVaddr = 0x199a7a0;
inline constexpr std::uint32_t kWidgetUpdateCode[4] = {0xd10443ffu, 0x6d0b23e9u, 0xa90c6bfcu, 0xa90d63f9u};

// NRadEngine::CInputListener::Pressed(state, id), the body behind the thunk at pinned 0x1037be0 (its `br` goes to
// pinned 0x2539920; ReVault 0x2639920). It reads the button table at state+0x80/+0x50 and returns bit 2 of the entry's
// flags at +0x230 (set for one input step when the button became active): the same predicate the PC splash skip
// reads at 0x140f9b770. The id arrives in x1; the decompilation does not show it. Reached like QuitOnError.
inline constexpr std::uint64_t kInputPressedVaddr = 0x2539920;
inline constexpr std::uint32_t kInputPressedCode[4] = {0xf940400au, 0xb400024au, 0xf9402808u, 0xaa1f03e9u};
using InputPressedSig = std::uint32_t(void* state, std::uint64_t id);

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

// NRadEngine::NRadGame::CR15NetGame::BeginLogIn(), defined in libr15 at 0x12917b8 (member: `this` in x0, no
// other argument, no return value). It is the task the RETRY page's script schedules on the NetGame (echovr.exe
// FUN_140dd9950 passes FUN_140157bd0, the same body, to FUN_140198460): it returns when the state is above 1,
// so "login failed" (-94) passes; it then calls LogIn ("[NETGAME] Logging in...") and switches to "logging
// in". Reached like QuitOnError: build ID, first four instructions, then base + this address.
// str x28, [sp, #-0x40]!; stp x23, x22, [sp, #0x10]; stp x21, x20, [sp, #0x20]; stp x19, x30, [sp, #0x30].
inline constexpr const char* kBeginLogInSymbol = "_ZN10NRadEngine8NRadGame11CR15NetGame10BeginLogInEv";
inline constexpr std::uint64_t kBeginLogInVaddr = 0x12917b8;
inline constexpr std::uint32_t kBeginLogInCode[4] = {0xf81c0ffc, 0xa9015bf7, 0xa90253f5, 0xa9037bf3};
using BeginLogInSig = void(CR15NetGameOpaque* self);

// NRadEngine::CncaGameSpace::SendComponentEvent(CSymbol64 event, SActor actor, CSymbol64 key, uint flag), defined
// in libr15 at 0x17fb0f8 (`this` = the game space in x0, event in x1, actor in x2, key in x3, flag in w4; no
// return value; ReVault lists it at 0x18fb0f8, its addresses for this module being 0x100000 above the loaded
// ones). QuitOnError calls it as (space, 0x60ba106e8f93f286, 0xffffffffffff, ~0, 1). The RETRY button of the
// login-failed page sends the graph event 0x8f0094c7bb02681c with the key 0x34f99a282ce78d72 (the third handler
// of script graph 7f2363b8976491aa's event_handler, which forks the BeginLogin and screen-tint timeline); the
// login prompt hook sends the same event once it has called BeginLogIn, so the page script that would run for
// the player's select runs (#672). Reached like QuitOnError: build ID, first four instructions, base + address.
// sub sp, sp, #0x40; mov x8, #-1; stp x8, x30, [sp, #0x28]; ldr x8, [x0, #0x58].
inline constexpr std::uint64_t kSendComponentEventVaddr = 0x17fb0f8;
inline constexpr std::uint32_t kSendComponentEventCode[4] = {0xd10103ff, 0x92800008, 0xa902fbe8, 0xf9402c08};
using SendComponentEventSig = void(void* space, std::uint64_t event, std::uint64_t actor, std::uint64_t key,
                                   std::uint32_t flag);
inline constexpr std::uint64_t kRetrySelectEvent = 0x8f0094c7bb02681cULL;
inline constexpr std::uint64_t kRetrySelectKey = 0x34f99a282ce78d72ULL;
inline constexpr std::uint64_t kAllActors = 0xffffffffffffULL;  // as QuitOnError passes it

// CR15NetGame fields the login-prompt hook reads and writes, measured on the pinned libr15:
namespace game_layout {
// EState at offset 0: SwitchTo (0x125b8b4) compares `ldr w0, [x0]` with the new state and stores
// it with `str w20, [x19]` (0x125b9f4). Names from GameStateString (0x124d478).
inline constexpr std::size_t kStateOffset = 0;
// The game space QuitOnError sends its event to: *(*(self + 8) + 0x7af0) (null before the game space exists).
inline constexpr std::size_t kGameOffset = 8;
inline constexpr std::size_t kGameSpaceOffset = 0x7af0;
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

// libr15.so's slot for CSysHttp::CreateConnection(unsigned long&, char const*), defined in libr15 itself
// (JUMP_SLOT 0x36e8028, readelf -rW on the pinned image).
inline GotTarget LibR15CreateConnection() {
  return {kLibR15, kCreateConnectionSymbol, RelocKind::kJumpSlot, kLibR15BuildId, 0x36e8028ULL};
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

// libr15.so's slot for android_main, defined in libr15 itself (JUMP_SLOT 0x36eb4c8, `readelf -rW` on the pinned
// image: `R_AARCH64_JUMP_SLOT 0xf784c4 android_main`).
inline GotTarget LibR15AndroidMain() {
  return {kLibR15, kAndroidMainSymbol, RelocKind::kJumpSlot, kLibR15BuildId, 0x36eb4c8ULL};
}

// libr15.so's slot for CSysInfo::RequestPermissions, defined in libr15 itself (JUMP_SLOT 0x36e4320, `readelf -rW` on
// the pinned image).
inline GotTarget LibR15RequestPermissions() {
  return {kLibR15, kRequestPermissionsSymbol, RelocKind::kJumpSlot, kLibR15BuildId, 0x36e4320ULL};
}

// libr15.so's slot for CSysInfo::CheckPermission, defined in libr15 itself (JUMP_SLOT 0x36c37c8, `readelf -rW` on the
// pinned image: `R_AARCH64_JUMP_SLOT 0xf87298 _ZN10NRadEngine8CSysInfo15CheckPermissionEPKc`).
inline GotTarget LibR15CheckPermission() {
  return {kLibR15, kCheckPermissionSymbol, RelocKind::kJumpSlot, kLibR15BuildId, 0x36c37c8ULL};
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

// libr15.so's slot for CSysInfo::GetDataRootDir, defined in libr15 itself (JUMP_SLOT 0x36f1df8, `readelf -rW` on the
// pinned image: `R_AARCH64_JUMP_SLOT 0xf87250 _ZN10NRadEngine8CSysInfo14GetDataRootDirEv`).
inline GotTarget LibR15GetDataRootDir() {
  return {kLibR15, kGetDataRootDirSymbol, RelocKind::kJumpSlot, kLibR15BuildId, 0x36f1df8ULL};
}

// libr15.so's slot for CSysModule::FindMethod, defined in libr15 itself (JUMP_SLOT 0x36db9f8, `readelf -rW` on the
// pinned image).
inline GotTarget LibR15FindMethod() {
  return {kLibR15, kFindMethodSymbol, RelocKind::kJumpSlot, kLibR15BuildId, 0x36db9f8ULL};
}

// libr15.so's slot for the VrApi device-extension import (JUMP_SLOT 0x36dd440, `readelf -rW` on the pinned image;
// imported, value 0).
inline GotTarget LibR15GetDeviceExtensionsVulkan() {
  return {kLibR15, kGetDeviceExtensionsVulkanSymbol, RelocKind::kJumpSlot, kLibR15BuildId, 0x36dd440ULL};
}

// libr15.so's slot for vrapi_GetSystemPropertyInt (JUMP_SLOT 0x36fa728, `readelf -rW` on the pinned image; imported,
// value 0).
inline GotTarget LibR15GetSystemPropertyInt() {
  return {kLibR15, kGetSystemPropertyIntSymbol, RelocKind::kJumpSlot, kLibR15BuildId, 0x36fa728ULL};
}

// VrApi.h: int vrapi_ShowSystemUI(const ovrJava* java, const ovrSystemUiType type). Called only from CVR::ShowSystemUI
// (ReVault 0x1b73990; ReVault VA = pinned VA + 0x100000), which always passes type 1. The PLT thunk at ReVault 0xffb730
// has one direct caller, 0x1b7399c (`revault_xrefs 0xffb730`). The JUMP_SLOT is ReVault 0x37d5280 (`revault_imports`),
// pinned 0x36d5280. Pin unverified until `just test-quest-hooks-pinned` runs on the real libr15.
using ShowSystemUISig = int(const OvrJavaOpaque* java, int type);
struct LibR15ShowSystemUITag {};
using LibR15ShowSystemUIThunk = CallbackThunk<LibR15ShowSystemUITag, ShowSystemUISig>;

inline GotTarget LibR15ShowSystemUI() {
  return {kLibR15, kShowSystemUISymbol, RelocKind::kJumpSlot, kLibR15BuildId, 0x36d5280ULL};
}

// The five VrApi imports the render-configuration log reads (docs/adr/0022-render-config-log.md). Each is an imported JUMP_SLOT
// (value 0) in the pinned libr15 (`readelf -rW`; ReVault shows every slot 0x100000 higher, `revault_imports`). None of the
// handlers changes an argument or a result.
//   vrapi_SetDisplayRefreshRate(ovrMobile*, float): one call site, CVR::UpdateTracking. JUMP_SLOT 0x36ec5f0.
//   vrapi_SetClockLevels(ovrMobile*, int cpu, int gpu): UpdateTracking, once per VR-mode entry. JUMP_SLOT 0x36c75f0.
//   vrapi_SetPropertyInt(const ovrJava*, ovrProperty, int): CVR::SetFoveationLevel (property 0xf) and
//     CVR::SetEnableDynamicFoveation (property 0x1e). JUMP_SLOT 0x36ffcf0.
//   vrapi_GetSystemPropertyFloat(const ovrJava*, ovrSystemProperty): property 4 is the refresh rate the runtime reports.
//     JUMP_SLOT 0x36c06f8.
//   vrapi_SubmitFrame2(ovrMobile*, const ovrSubmitFrameDescription2*): one call per rendered frame. ReVault slot 0x37e7010
//     (`revault_imports`), pinned 0x36e7010.
struct OvrMobileOpaque;
struct OvrSubmitFrameDescriptionOpaque;
using SetDisplayRefreshRateSig = int(OvrMobileOpaque* ovr, float rate);
struct LibR15SetDisplayRefreshRateTag {};
using LibR15SetDisplayRefreshRateThunk = CallbackThunk<LibR15SetDisplayRefreshRateTag, SetDisplayRefreshRateSig>;
using SetClockLevelsSig = int(OvrMobileOpaque* ovr, int cpuLevel, int gpuLevel);
struct LibR15SetClockLevelsTag {};
using LibR15SetClockLevelsThunk = CallbackThunk<LibR15SetClockLevelsTag, SetClockLevelsSig>;
using SetPropertyIntSig = int(const OvrJavaOpaque* java, int property, int value);
struct LibR15SetPropertyIntTag {};
using LibR15SetPropertyIntThunk = CallbackThunk<LibR15SetPropertyIntTag, SetPropertyIntSig>;
using GetSystemPropertyFloatSig = float(const OvrJavaOpaque* java, int property);
struct LibR15GetSystemPropertyFloatTag {};
using LibR15GetSystemPropertyFloatThunk = CallbackThunk<LibR15GetSystemPropertyFloatTag, GetSystemPropertyFloatSig>;
using SubmitFrame2Sig = int(OvrMobileOpaque* ovr, const OvrSubmitFrameDescriptionOpaque* description);
struct LibR15SubmitFrame2Tag {};
using LibR15SubmitFrame2Thunk = CallbackThunk<LibR15SubmitFrame2Tag, SubmitFrame2Sig>;

inline GotTarget LibR15SetDisplayRefreshRate() {
  return {kLibR15, kSetDisplayRefreshRateSymbol, RelocKind::kJumpSlot, kLibR15BuildId, 0x36ec5f0ULL};
}
inline GotTarget LibR15SetClockLevels() {
  return {kLibR15, kSetClockLevelsSymbol, RelocKind::kJumpSlot, kLibR15BuildId, 0x36c75f0ULL};
}
inline GotTarget LibR15SetPropertyInt() {
  return {kLibR15, kSetPropertyIntSymbol, RelocKind::kJumpSlot, kLibR15BuildId, 0x36ffcf0ULL};
}
inline GotTarget LibR15GetSystemPropertyFloat() {
  return {kLibR15, kGetSystemPropertyFloatSymbol, RelocKind::kJumpSlot, kLibR15BuildId, 0x36c06f8ULL};
}
inline GotTarget LibR15SubmitFrame2() {
  return {kLibR15, kSubmitFrame2Symbol, RelocKind::kJumpSlot, kLibR15BuildId, 0x36e7010ULL};
}

// libr15.so's slot for CResourceManager::FillResourceFileRequest, defined in libr15 itself (JUMP_SLOT 0x36efa80,
// `readelf -rW` on the pinned image; the PLT stub at 0xf30730 loads it).
inline GotTarget LibR15FillResourceFileRequest() {
  return {kLibR15, kFillResourceFileRequestSymbol, RelocKind::kJumpSlot, kLibR15BuildId, 0x36efa80ULL};
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
