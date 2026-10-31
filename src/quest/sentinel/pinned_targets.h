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

#include <signal.h>
#include <time.h>

#include <cstddef>
#include <cstdint>

#include "callback_thunk.h"
#include "got_hook.h"
#include "libr15_identity.h"

namespace sentinel::pinned {

inline constexpr const char* kLibR15 = "libr15.so";
// The GNU build ID of the STOCK libr15.so in the store APK (the file the patcher reads, the plugins declare and the pinned
// tests open); equal to nevr_quest::patch::kLibR15StockBuildId (static_assert in libr15_identity.h). It is a literal here
// because the plugin fixture's CMake and tools/quest_apk_patch_map.py read it from this line.
inline constexpr const char* kLibR15StockBuildId = "b243509c08ce677aeb95fa348016949b3fc45230";
// What the running process checks the live libr15 note against is LibR15PatchedBuildId() (libr15_identity.h): the ID of
// the file the build-time patcher wrote (docs/adr/0021 section 7).
inline constexpr const char* kMatchmaking = "libpnsradmatchmaking.so";
inline constexpr const char* kMatchmakingBuildId = "8c4fddc079eae65909530132a56c48da48b2708c";

// The game's script library that builds the login-failed page's title (#1137). It has no GNU build ID and no SONAME, so
// the target pins the module file name, the symbol and the link-time slot address (`readelf -rW` on the store APK's
// copy: JUMP_SLOT 0x12dd0 `_ZN8string_tC2EPKc`, the library's only relocation of that symbol; BIND_NOW).
inline constexpr const char* kLoginScriptLib = "lib7f2363b8976491aa.so";
inline constexpr const char* kStringTCtorSymbol = "_ZN8string_tC2EPKc";

inline constexpr const char* kTStringSymbol = "_ZNK10NRadEngine5CJson7TStringEPKcS2_j";
inline constexpr const char* kClockGettimeSymbol = "clock_gettime";
inline constexpr const char* kSigactionSymbol = "sigaction";
inline constexpr const char* kSignalSymbol = "signal";
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
inline constexpr const char* kSetDisplayRefreshRateSymbol = "vrapi_SetDisplayRefreshRate";
inline constexpr const char* kSetClockLevelsSymbol = "vrapi_SetClockLevels";
inline constexpr const char* kSetPropertyIntSymbol = "vrapi_SetPropertyInt";
inline constexpr const char* kGetSystemPropertyFloatSymbol = "vrapi_GetSystemPropertyFloat";
inline constexpr const char* kSubmitFrame2Symbol = "vrapi_SubmitFrame2";
inline constexpr const char* kGetCurrentInputStateSymbol = "vrapi_GetCurrentInputState";
inline constexpr const char* kGetInputDeviceCapabilitiesSymbol = "vrapi_GetInputDeviceCapabilities";
inline constexpr const char* kEnablePageNodeEnterSymbol =
    "_ZN10NRadEngine8NRadGame25CR15UIPage2EnablePageNode5EnterERKNS0_29SR15UIPage2EnablePageNodeDataE";

// ---- typed callbacks --------------------------------------------------------

// libc: int clock_gettime(clockid_t, struct timespec*).
using ClockGettimeSig = int(clockid_t, struct timespec*);
struct ClockGettimeTag {};
using ClockGettimeThunk = CallbackThunk<ClockGettimeTag, ClockGettimeSig>;

// libc: int sigaction(int, const struct sigaction*, struct sigaction*) and sighandler_t signal(int, sighandler_t), the two
// ways libr15 installs a signal handler (#1605). libr15 imports both from libc (`readelf -rW` on the store file:
// R_AARCH64_JUMP_SLOT 0x36f3b68 sigaction and 0x36e6508 signal; BIND_NOW). `bl` to the sigaction stub (0xf38900) appears 80
// times in the store file's .text (objdump -d): the 31-signal save and restore in the OpenSSL console UI (0x1133b60 to
// 0x1133d14 and 0x1133e58 on), calls with signal 13 and a null old action (0x100185c, 0x1004f28, 0x10069a4, 0x100af08) and loops over a table of signal numbers (0xfb111c, 0xfb1bc4, 0xffe41c); `bl`
// to the signal stub (0xf1dc40) appears once, in that console UI (signal 28). The crash-first hook answers these for the
// signals the sentinel's crash handler owns and passes every other signal through (crash_first_hook.h).
using SigactionSig = int(int signo, const struct sigaction* act, struct sigaction* old);
struct LibR15SigactionTag {};
using LibR15SigactionThunk = CallbackThunk<LibR15SigactionTag, SigactionSig>;
using SignalHandlerFn = void (*)(int);
using SignalSig = SignalHandlerFn(int signo, SignalHandlerFn handler);
struct LibR15SignalTag {};
using LibR15SignalThunk = CallbackThunk<LibR15SignalTag, SignalSig>;

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

// string_t::string_t(char const*), defined in the login-failed page's script library itself (ReVault 0x10df98, 44 bytes:
// NRadString::Copy(this, text, 0x40)), reached only through that library's PLT stub (ReVault 0x104a90), whose one caller
// is execute_node_cfa11df1 (ReVault 0x10d0e8), the node that builds the literal "LOGIN FAILED" and hands it to
// EXTERNALFUNCTIONNODE::setparam as the page's title. `self` (x0) is the string_t, a 64-byte inline buffer the
// constructor fills; `text` (x1) is copied, never kept. The ReVault VA is the file vaddr + 0x100000.
struct StringTOpaque;
using StringTCtorSig = void(StringTOpaque* self, const char* text);
struct LoginScriptStringTag {};
using LoginScriptStringThunk = CallbackThunk<LoginScriptStringTag, StringTCtorSig>;

inline GotTarget LoginScriptStringCtor() {
  return {kLoginScriptLib, kStringTCtorSymbol, RelocKind::kJumpSlot, nullptr, 0x12dd0ULL};
}

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
// NRadEngine::CVR::RefreshRTSize() (libr15 0x1a709a0, no argument, no result), the function that makes the eye-size query. The
// reference build's eye size is decided on this function's JUMP_SLOT (0x36dbd10, texture_quality_hook.h, #1737 C4).
inline constexpr const char* kRefreshRTSizeSymbol = "_ZN10NRadEngine3CVR13RefreshRTSizeEv";
using RefreshRTSizeSig = void();
struct LibR15RefreshRTSizeTag {};
using LibR15RefreshRTSizeThunk = CallbackThunk<LibR15RefreshRTSizeTag, RefreshRTSizeSig>;
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

// The libr15 functions the loose-file overlay calls (resource_request_hook.cpp). None is hooked: the overlay resolves each
// by its symbol and proves the address is the load base plus the pinned vaddr. ReVault shows every vaddr 0x100000 higher.
//   ClearFileRequestData(SFileRequestData&) (0x2a9759c): the first call of FillResourceFileRequest; it sets the request's
//     +0x10 and +0x418 to -1 and clears the rest.
//   CResourceManager::GetUnpackagedResourcePath(CSymbol64 type, CSymbol64 name, CFixedStringT<512>&) const (0x25441b8): writes
//     the loose file's path into the request's +0x18 buffer; returns 0 on success.
//   CSysFile::Open(char const*, unsigned int flags, long& handle) (0xf85538, static): flags 1 is O_RDONLY; returns 0 on success.
//   CSysFile::Size(long handle, unsigned long long* size) (0xf859fc, static): returns 0 on success.
inline constexpr const char* kClearFileRequestDataSymbol =
    "_ZN10NRadEngine20ClearFileRequestDataERNS_16SFileRequestDataE";
inline constexpr std::uint64_t kClearFileRequestDataVaddr = 0x2a9759c;
inline constexpr const char* kGetUnpackagedResourcePathSymbol =
    "_ZNK10NRadEngine16CResourceManager25GetUnpackagedResourcePathENS_9CSymbol64ES1_RNS_13CFixedStringTILy512EEE";
inline constexpr std::uint64_t kGetUnpackagedResourcePathVaddr = 0x25441b8;
inline constexpr const char* kCSysFileOpenSymbol = "_ZN10NRadEngine8CSysFile4OpenEPKcjPl";
inline constexpr std::uint64_t kCSysFileOpenVaddr = 0xf85538;
inline constexpr const char* kCSysFileSizeSymbol = "_ZN10NRadEngine8CSysFile4SizeElPy";
inline constexpr std::uint64_t kCSysFileSizeVaddr = 0xf859fc;

// SFileRequestData (0x470 bytes). The log reads the fields after a hit (ClearFileRequestData, 0x2a9759c, sets +0x10 and +0x418
// to -1; the hit overwrites both). Measured on the pinned libr15 from the function's own stores (0x2543a2c-0x2543bf8). The loose
// file overlay writes the fields from kOwner to kValid on a miss (resource_request_hook.cpp, ServeLoose).
namespace resource_request_layout {
inline constexpr std::size_t kRequestBytes = 0x470;
inline constexpr std::size_t kOwner = 0x0;               // u64: the owner (the CResource* for a base load), stored by the stock fill
inline constexpr std::size_t kIndex = 0x8;               // u64: the buffer or mip index, stored by the stock fill
inline constexpr std::size_t kPackageHandle = 0x10;      // i64: the package file handle, -1 when none
inline constexpr std::size_t kLoosePath = 0x18;          // char[512]: the loose file's path (GetUnpackagedResourcePath)
inline constexpr std::size_t kChunkIndex = 0x418;        // i64: the manifest record's chunk index, -1 when none
inline constexpr std::size_t kChunkFileOffset = 0x420;   // u64: the chunk's offset in the package file
inline constexpr std::size_t kChunkCompressed = 0x428;   // u64: the chunk's compressed size
inline constexpr std::size_t kChunkDecompressed = 0x430; // u64: the chunk's decompressed size
inline constexpr std::size_t kOffsetInChunk = 0x438;     // u64: the resource's offset inside the chunk
inline constexpr std::size_t kSize = 0x440;              // u64: the resource's size
inline constexpr std::size_t kUncompressedSize = 0x448;  // u64: the resource's size as the stock hit path stores it
inline constexpr std::size_t kAlignment = 0x450;         // u64: the record's alignment
inline constexpr std::size_t kMemSpace = 0x460;          // u64: the allocator the request was made with
inline constexpr std::size_t kValid = 0x468;             // u32: 1 on a hit
}  // namespace resource_request_layout

// The manifest CResourceManager::FillResourceFileRequest searches (0x2543a30-0x2543a90). The manager's pointer at +8 is the
// manifest; the manifest holds the record array at +0 and the record count at +0x30. A record is 32 bytes: the type (i64, the
// first key compared), the name (i64, compared when the types are equal), then the chunk and offsets. The search compares
// both keys as signed 64-bit values and the count as unsigned (resource_request_hook.cpp, ManifestMisses).
namespace resource_manifest_layout {
inline constexpr std::size_t kManagerManifest = 0x8;
inline constexpr std::size_t kManifestRecords = 0x0;
inline constexpr std::size_t kManifestCount = 0x30;
inline constexpr std::size_t kRecordShift = 5;  // 32-byte records
inline constexpr std::size_t kRecordType = 0x0;
inline constexpr std::size_t kRecordName = 0x8;
}  // namespace resource_manifest_layout

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

// NRadEngine::CResourceManager::AsyncResourceIOCallback(SIORequestCallbackData&), defined in libr15 at 0x2543448 (484
// bytes; ReVault lists it at 0x2643448): what the streaming reader calls when the bytes of a resource are in memory
// (#966). The game never calls it by name: ResourceLoadInternal loads its address from the data slot GLOB_DAT 0x37195b8
// (`ldr x4, [x4, #0x5b8]` at 0x254391c and 0x25439ac) and passes it to CStreamingIOProcessor::ReadFileAsync, which keeps
// it in the request context (+0x4b8) and calls it with the context's callback block (+0x478). The slot is read at each
// ReadFileAsync, so a replacement is used by every later load. The block layout is in menu_cards_swap.h; the function
// returns its status in x0 (read here as a 64-bit value and passed through untouched).
using AsyncResourceIOCallbackSig = std::uint64_t(unsigned char* callbackBlock);
struct LibR15AsyncResourceIOCallbackTag {};
using LibR15AsyncResourceIOCallbackThunk = CallbackThunk<LibR15AsyncResourceIOCallbackTag, AsyncResourceIOCallbackSig>;
inline constexpr const char* kAsyncResourceIOCallbackSymbol =
    "_ZN10NRadEngine16CResourceManager23AsyncResourceIOCallbackERNS_22SIORequestCallbackDataE";
// NRadEngine::CMemoryContext::context, a data symbol libr15 defines (.dynsym, 8 bytes at 0x37688c0): the pointer to the
// memory context. FreeResource releases a resource's buffer 0 through the allocator at context + 0x10.
inline constexpr const char* kMemoryContextSymbol = "_ZN10NRadEngine14CMemoryContext7contextE";
inline constexpr std::uint64_t kMemoryContextVaddr = 0x37688c0;

// NRadEngine::NRadGame::NR15Logger::MetricLog(char const* json, unsigned long long length), defined in libr15 at 0x120cc50
// (the store file: `aarch64-linux-gnu-objdump -d`, the symbol's own value in `.dynsym`). The json pointer arrives in x0 and
// its length in x1 (`mov x19, x1` at 0x120cc88 and `mov x20, x0` at 0x120cc8c). Its one caller in libr15 is
// CR15NetJsonLog::WriteLog, so it runs once per metric event. It returns at once unless two flag bytes are set (U2 in
// the Quest plan: a headset run counts the calls). The observer (quest/invite/wizard_metric_hook.h, ADR 0017) installs on
// the JUMP_SLOT libr15's own call sites load; the slot holds one entry for the symbol in the store file's relocation table.
inline constexpr const char* kMetricLogSymbol = "_ZN10NRadEngine8NRadGame10NR15Logger9MetricLogEPKcy";
inline constexpr std::uint64_t kMetricLogVaddr = 0x120cc50;
inline constexpr std::uint64_t kMetricLogSlotVaddr = 0x36da2c8;
// The first four instructions, as the observer checks them before it installs (`stp x25, x24, [sp, #-64]!` and three
// more `stp`s; the store file at 0x120cc50 through 0x120cc5c).
inline constexpr std::uint32_t kMetricLogCode[4] = {0xa9bc63f9, 0xa9015bf7, 0xa90253f5, 0xa9037bf3};
using MetricLogSig = void(const char* json, unsigned long long length);
struct LibR15MetricLogTag {};
using LibR15MetricLogThunk = CallbackThunk<LibR15MetricLogTag, MetricLogSig>;

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
// NativeActivity, then returns. That JNI call passes the array and no request code (#947, permission_request.h): the
// permission-wait hook makes the request itself instead. It keeps no state and does not wait for the answer; the boot
// call in CR15Game::Initialize is followed at once by CncaGame::Initialize, whose package-manifest read needs the
// storage permissions (#640). The three names it gets there are RECORD_AUDIO, READ_EXTERNAL_STORAGE, WRITE_EXTERNAL_STORAGE.
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

// NRadEngine::CVR::ShowSystemUI(), defined in libr15 at 0x1a73990 (exported; ReVault 0x1b73990): `mov w1, #1` and a tail
// call of vrapi_ShowSystemUI(&java, 1). libr15 calls it through its own PLT (stub 0xf47880, JUMP_SLOT 0x36fb328) from
// exactly one place, state 0 of CR15VRMenuWidgetCS::UpdateExclusive (bl at 0x1f99ca0), the menu's QUIT; state 1 of that
// switch calls CR15Game::QuitToMain. No other library in the APK imports it (`readelf -rW` over all 483).
inline constexpr const char* kShowSystemUISymbol = "_ZN10NRadEngine3CVR12ShowSystemUIEv";
inline constexpr std::uint64_t kShowSystemUIVaddr = 0x1a73990;
inline constexpr std::uint64_t kShowSystemUIStub = 0xf47880;
inline constexpr std::uint64_t kShowSystemUICallSite = 0x1f99ca0;  // CR15VRMenuWidgetCS::UpdateExclusive, state 0
using ShowSystemUISig = void();
struct LibR15ShowSystemUITag {};
using LibR15ShowSystemUIThunk = CallbackThunk<LibR15ShowSystemUITag, ShowSystemUISig>;

// NRadEngine::CSysInfo::OSBuildVersion(unsigned long long&), defined in libr15 at 0xf875c4 (184 bytes, exported; ReVault
// 0x10875c4): reads the system property ro.build.version.incremental, and when it is at most 20 characters of decimal
// digits stores the number through its argument and returns 1; otherwise it returns 0 and stores nothing. The return is
// w0, 0 or 1. libr15 calls it through its own PLT (stub 0xf505a0, JUMP_SLOT 0x36ff9b8) from exactly one place,
// CR15NetGame::Initialize (bl at 0x1285ff8): when it returns 1 and the number is below kMinOSBuildVersion, Initialize
// switches the net game to state -100, "os needs update" (CR15NetGame::GameStateString), and never reaches the login.
// libpnsovr has its own copy (its GotUserProofCB puts the number into the login's system info); this slot does not
// reach it.
inline constexpr const char* kOSBuildVersionSymbol = "_ZN10NRadEngine8CSysInfo14OSBuildVersionERy";
inline constexpr std::uint64_t kOSBuildVersionVaddr = 0xf875c4;
inline constexpr std::uint64_t kOSBuildVersionStub = 0xf505a0;
inline constexpr std::uint64_t kOSBuildVersionCallSite = 0x1285ff8;  // CR15NetGame::Initialize
using OSBuildVersionSig = int(std::uint64_t* version);
struct LibR15OSBuildVersionTag {};
using LibR15OSBuildVersionThunk = CallbackThunk<LibR15OSBuildVersionTag, OSBuildVersionSig>;
// NRadEngine::kMinOSBuildVersion, the floor Initialize compares with (an 8-byte constant in .rodata, exported; Initialize
// reads it through GLOB_DAT 0x371bd38). Its value in the pinned build is kMinOSBuildVersionValue.
inline constexpr const char* kMinOSBuildVersionSymbol = "_ZN10NRadEngine18kMinOSBuildVersionE";
inline constexpr std::uint64_t kMinOSBuildVersionVaddr = 0x2b9ca88;
inline constexpr std::uint64_t kMinOSBuildVersionValue = 6023800249000000ULL;

// The platform provider (#1220, docs/adr/0026). libr15 reaches a provider only through its own JUMP_SLOTs; each function
// below is defined in libr15 and called through libr15's PLT stub, from the call sites listed (got_pinned_test.cpp proves
// the stub loads the slot and that these are its only `b`/`bl` callers).
//
// NRadEngine::CNSProvider::Create(unsigned long& handle, char const* name, void const* json, SNSProviderCallbacks const&)
// (0x192f3e4): unloads a nonzero `handle`, loads the module `name` (CModuleLoader::Load, which keeps a module already
// loaded without running its RadPlugin* entries again) and stores its handle; `json` is a CJson* the module's RadPluginMain
// reads. Returns 0 on success (w0). CR15Game::Initialize calls it twice: the platform provider at 0x11f9f80 (`pnsovr`, or
// `pnsdemo` under -noovr, with a json of appid, lobbyversion, headsettype, activity and javaenv) and `pnsrad` at 0x11f9fdc
// with a null json.
inline constexpr const char* kProviderCreateSymbol =
    "_ZN10NRadEngine11CNSProvider6CreateERmPKcPKvRKNS_20SNSProviderCallbacksE";
inline constexpr std::uint64_t kProviderCreateVaddr = 0x192f3e4;
inline constexpr std::uint64_t kProviderCreateStub = 0xf31580;
inline constexpr std::uint64_t kProviderCreateCallSites[] = {0x11f9f80, 0x11f9fdc};  // CR15Game::Initialize
using ProviderCreateSig = int(unsigned long* handle, const char* name, const void* json, const void* callbacks);
struct LibR15ProviderCreateTag {};
using LibR15ProviderCreateThunk = CallbackThunk<LibR15ProviderCreateTag, ProviderCreateSig>;
// NRadEngine::CNSProvider::Destroy(unsigned long handle) (0x192f5c0): `cbz x0; b CModuleLoader::Unload`, which calls the
// module's RadPluginShutdown and then CSysModule::Unload(handle, 0), the dlclose (0x281a084-0x281a0c0). CR15Game::
// ShutdownEngine calls it on the pnsdemo, pnsrad and pnsovr handles in that order (0x11f500c, 0x11f5014, 0x11f501c).
inline constexpr const char* kProviderDestroySymbol = "_ZN10NRadEngine11CNSProvider7DestroyEm";
inline constexpr std::uint64_t kProviderDestroyVaddr = 0x192f5c0;
inline constexpr std::uint64_t kProviderDestroyStub = 0xf537e0;
inline constexpr std::uint64_t kProviderDestroyCallSites[] = {0x11f500c, 0x11f5014, 0x11f501c};  // ShutdownEngine
using ProviderDestroySig = void(unsigned long handle);
struct LibR15ProviderDestroyTag {};
using LibR15ProviderDestroyThunk = CallbackThunk<LibR15ProviderDestroyTag, ProviderDestroySig>;
// NRadEngine::CSysModule::Unload(unsigned long handle, unsigned int) (0x2a9e4a0, exported): with 0 as the second argument
// it is the dlclose alone. The Destroy hook calls it for a handle whose module was already shut down.
inline constexpr const char* kSysModuleUnloadSymbol = "_ZN10NRadEngine10CSysModule6UnloadEmj";
inline constexpr std::uint64_t kSysModuleUnloadVaddr = 0x2a9e4a0;
using SysModuleUnloadFn = void (*)(unsigned long handle, unsigned int how);
// NRadEngine::CJson::TString(char const* path, char const* fallback, unsigned int) const (0xfa2e7c, exported): the string
// at `path`, else `fallback`. The Create hook reads the platform json's appid with it before it drops that json.
inline constexpr const char* kCJsonTStringSymbol = "_ZNK10NRadEngine5CJson7TStringEPKcS2_j";
inline constexpr std::uint64_t kCJsonTStringVaddr = 0xfa2e7c;
using CJsonTStringFn = const char* (*)(const void* json, const char* path, const char* fallback, unsigned int flags);
// NRadEngine::CNSProvider::ProviderID(unsigned long) (0x192f27c) and UserProviderID(unsigned long) (0x192f330): the
// module's `ProviderID`/`UserProviderID` export, called with no argument, or -1 when the handle is 0 or the module has none.
// The return is the provider's CSymbol64 in x0. ProviderID is called from 9 places (lobby Find/Join, PartyAddDemoMember),
// UserProviderID from 19 (party, invite, friend-request UI); none is a hook target by itself, so no call site is pinned.
inline constexpr const char* kProviderIdSymbol = "_ZN10NRadEngine11CNSProvider10ProviderIDEm";
inline constexpr std::uint64_t kProviderIdVaddr = 0x192f27c;
inline constexpr std::uint64_t kProviderIdStub = 0xf2b4e0;
inline constexpr std::size_t kProviderIdCallers = 9;
inline constexpr const char* kUserProviderIdSymbol = "_ZN10NRadEngine11CNSProvider14UserProviderIDEm";
inline constexpr std::uint64_t kUserProviderIdVaddr = 0x192f330;
inline constexpr std::uint64_t kUserProviderIdStub = 0xf34a80;
inline constexpr std::size_t kUserProviderIdCallers = 19;
using ProviderIdSig = std::uint64_t(unsigned long handle);
struct LibR15ProviderIdTag {};
struct LibR15UserProviderIdTag {};
using LibR15ProviderIdThunk = CallbackThunk<LibR15ProviderIdTag, ProviderIdSig>;
using LibR15UserProviderIdThunk = CallbackThunk<LibR15UserProviderIdTag, ProviderIdSig>;

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

// CR15VRMenuWidgetCS::Update(), the pointer-ray menu widget's per-frame update (ReVault 0x209a7a0), is a row of the
// build-time patch table (`splash_skip.widget_update`, quest/patch/libr15_patch_table.h): the engine registers its
// address (R15RegisterComponentSystemUpdates), so no GOT slot names it, and the splash skip activates the row. The
// widget fields the hook reads (ctx +0x80, defs +0xb8, dense +0xf8, links +0x100, bounds +0xf2/+0xf4, input +0x138) are
// the ones the update itself reads (the PC twin's offsets, checked against this decompilation). The table's stock words
// at the site are checked against the store file by libr15_patcher_pinned_test.

// NRadEngine::CGTextureResource::ForceLoadAllMips (libr15 0x18e674c pinned; ReVault 0x19e674c): the function the UI texture
// hook calls. Its first four words are `sub sp,sp,#0xd0; str x20,[sp,#0xb0]; stp x19,x30,[sp,#0xc0]; ldr x8,[x0,#0x70]`.
inline constexpr std::uint64_t kForceLoadAllMipsVaddr = 0x18e674c;
inline constexpr std::uint32_t kForceLoadAllMipsCode[4] = {0xD10343FFu, 0xF9005BF4u, 0xA90C7BF3u, 0xF9403808u};

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
// The main-menu join (#966 S4, docs/adr/0017). CR15Game::BeginMultiplayer() (libr15 0x1201be4; ReVault 0x1301be4, the 52-byte
// wrapper that calls the settings body through a thunk) takes the game object in x0 and begins the staged match. Its first 16
// bytes, read from libr15 in ReVault (revault_read_memory 0x1301be4, 16 bytes).
inline constexpr std::uint64_t kBeginMultiplayerVaddr = 0x1201be4;
inline constexpr std::uint32_t kBeginMultiplayerCode[4] = {0xd10183ff, 0xa904fbff, 0xd2c00068, 0x92800009};
// CR15Game::EnableTransitionMenu (libr15 0x12005fc; ReVault 0x13005fc, the plan's "mp_transition"): sends one component event to
// the menu gamespace (CR15Game+0x7af0) and runs before BeginMultiplayer. First 16 bytes from ReVault 0x13005fc.
inline constexpr std::uint64_t kEnableTransitionMenuVaddr = 0x12005fc;
inline constexpr std::uint32_t kEnableTransitionMenuCode[4] = {0xd10103ff, 0xa9037bf3, 0xf97d7813, 0xb40001d3};
using BeginLogInSig = void(CR15NetGameOpaque* self);

// NRadEngine::CncaGameSpace::SendComponentEvent(CSymbol64 event, SActor actor, CSymbol64 key, uint flag), defined
// in libr15 at 0x17fb0f8 (`this` = the game space in x0, event in x1, actor in x2, key in x3, flag in w4; no
// return value; ReVault lists it at 0x18fb0f8, its addresses for this module being 0x100000 above the loaded
// ones). QuitOnError calls it as (space, 0x60ba106e8f93f286, 0xffffffffffff, ~0, 1). The login prompt hook sends
// evt_btn_clicked (kRetrySelectEvent) with the key of the login-failed page's btn_exit branch (kLoginPageExitKey) and
// the actor kAllActors. It sends this select alone: BeginLogIn runs only in the hook's fallback, when the select is
// unavailable or has no effect 2 s after it (kRetryFallbackNs, login_prompt_hook.cpp). That key is not RETRY's (issue
// #1391, follow-up RE: the branch's thread body is empty). Whether the send reaches the page script is not established
// (#1391). Reached like QuitOnError: build ID, first four instructions, base + address.
// sub sp, sp, #0x40; mov x8, #-1; stp x8, x30, [sp, #0x28]; ldr x8, [x0, #0x58].
inline constexpr std::uint64_t kSendComponentEventVaddr = 0x17fb0f8;
inline constexpr std::uint32_t kSendComponentEventCode[4] = {0xd10103ff, 0x92800008, 0xa902fbe8, 0xf9402c08};
using SendComponentEventSig = void(void* space, std::uint64_t event, std::uint64_t actor, std::uint64_t key,
                                   std::uint32_t flag);
// evt_btn_clicked: the event the login-failed page's script handler accepts (issue #1391, follow-up RE).
inline constexpr std::uint64_t kRetrySelectEvent = 0x8f0094c7bb02681cULL;
// CSymbol64("btn_exit"): the key of the login-failed page's btn_exit branch, not RETRY's. Its handler starts a thread
// whose body returns at once (issue #1391, follow-up RE).
inline constexpr std::uint64_t kLoginPageExitKey = 0x34f99a282ce78d72ULL;
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
  return {kLibR15, kClockGettimeSymbol, RelocKind::kJumpSlot, LibR15PatchedBuildId(), 0x36c33e8ULL};
}

// libr15.so's slots for libc's sigaction (JUMP_SLOT 0x36f3b68) and signal (JUMP_SLOT 0x36e6508), from `readelf -rW` on the
// store file (#1605).
inline GotTarget LibR15Sigaction() {
  return {kLibR15, kSigactionSymbol, RelocKind::kJumpSlot, LibR15PatchedBuildId(), 0x36f3b68ULL};
}
inline GotTarget LibR15Signal() {
  return {kLibR15, kSignalSymbol, RelocKind::kJumpSlot, LibR15PatchedBuildId(), 0x36e6508ULL};
}

// The provider libraries the game loads after the constructor (CNSProvider::Create -> CSysModule::Load -> dlopen; none is a
// DT_NEEDED of libr15: `readelf -d`) and that import libc's sigaction and signal as well (#1605). Slots are the link-time
// addresses of the JUMP_SLOTs in the store APK's copies (`readelf -rW`; each library is BIND_NOW and has one relocation of each
// symbol). libpnsovr is listed because the default Quest build still loads it (docs/adr/0026 is not implemented: the rename
// hook is off by default and the login hook installs on libpnsovr); the hook costs nothing when it is not loaded.
inline constexpr const char* kPnsrad = "libpnsrad.so";
inline constexpr const char* kPnsradBuildId = "c58fb82e42d9ed6564744cfa10f05af74aedd0ee";
inline constexpr const char* kPnsovr = "libpnsovr.so";
inline constexpr const char* kPnsovrBuildId = "ca47bb8d03e6f43c1825133bbb9c15f174705c51";
struct CrashLateModule {
  const char* module;
  const char* buildId;
  std::uint64_t sigactionSlot;
  std::uint64_t signalSlot;
};
inline constexpr CrashLateModule kCrashLateModules[] = {
    {kPnsrad, kPnsradBuildId, 0x72de30ULL, 0x732df0ULL},
    {kMatchmaking, kMatchmakingBuildId, 0x6b6768ULL, 0x6bb350ULL},
    {kPnsovr, kPnsovrBuildId, 0x6dd218ULL, 0x6e2130ULL},
};
inline GotTarget CrashLateSigaction(const CrashLateModule& m) {
  return {m.module, kSigactionSymbol, RelocKind::kJumpSlot, m.buildId, m.sigactionSlot};
}
inline GotTarget CrashLateSignal(const CrashLateModule& m) {
  return {m.module, kSignalSymbol, RelocKind::kJumpSlot, m.buildId, m.signalSlot};
}

// libr15.so's slot for CJson::TString, defined in libr15 itself (JUMP_SLOT 0x36ebe08).
inline GotTarget LibR15TString() {
  return GotTarget{kLibR15, kTStringSymbol, RelocKind::kJumpSlot, LibR15PatchedBuildId(), 0x36ebe08ULL}.ServiceSide();  // #1737
}

// libr15.so's slot for CSysHttp::CreateConnection(unsigned long&, char const*), defined in libr15 itself
// (JUMP_SLOT 0x36e8028, readelf -rW on the pinned image).
inline GotTarget LibR15CreateConnection() {
  return GotTarget{kLibR15, kCreateConnectionSymbol, RelocKind::kJumpSlot, LibR15PatchedBuildId(), 0x36e8028ULL}.ServiceSide();  // #1737
}

// libr15.so's slot for CR15NetGame::SetDelimitedErrorMessage, defined in libr15 itself
// (PLT 0xf23510 loads JUMP_SLOT 0x36e9170).
inline GotTarget LibR15SetDelimitedErrorMessage() {
  return {kLibR15, kSetDelimitedErrorMessageSymbol, RelocKind::kJumpSlot, LibR15PatchedBuildId(), 0x36e9170ULL};
}

// libr15.so's slot for CR15NetGame::Update, defined in libr15 itself (PLT 0xf11e20 loads
// JUMP_SLOT 0x36e05f8).
inline GotTarget LibR15NetGameUpdate() {
  return {kLibR15, kNetGameUpdateSymbol, RelocKind::kJumpSlot, LibR15PatchedBuildId(), 0x36e05f8ULL};
}

// libr15.so's slot for android_main, defined in libr15 itself (JUMP_SLOT 0x36eb4c8, `readelf -rW` on the pinned
// image: `R_AARCH64_JUMP_SLOT 0xf784c4 android_main`).
inline GotTarget LibR15AndroidMain() {
  return {kLibR15, kAndroidMainSymbol, RelocKind::kJumpSlot, LibR15PatchedBuildId(), 0x36eb4c8ULL};
}

// libr15.so's slot for CSysInfo::RequestPermissions, defined in libr15 itself (JUMP_SLOT 0x36e4320, `readelf -rW` on
// the pinned image).
inline GotTarget LibR15RequestPermissions() {
  return {kLibR15, kRequestPermissionsSymbol, RelocKind::kJumpSlot, LibR15PatchedBuildId(), 0x36e4320ULL};
}

// libr15.so's slot for CSysInfo::CheckPermission, defined in libr15 itself (JUMP_SLOT 0x36c37c8, `readelf -rW` on the
// pinned image: `R_AARCH64_JUMP_SLOT 0xf87298 _ZN10NRadEngine8CSysInfo15CheckPermissionEPKc`).
inline GotTarget LibR15CheckPermission() {
  return {kLibR15, kCheckPermissionSymbol, RelocKind::kJumpSlot, LibR15PatchedBuildId(), 0x36c37c8ULL};
}

// libr15.so's slot for CVR::ShowSystemUI, defined in libr15 itself (JUMP_SLOT 0x36fb328, `readelf -rW` on the pinned
// image: `R_AARCH64_JUMP_SLOT 0x1a73990 _ZN10NRadEngine3CVR12ShowSystemUIEv`).
inline GotTarget LibR15ShowSystemUI() {
  return {kLibR15, kShowSystemUISymbol, RelocKind::kJumpSlot, LibR15PatchedBuildId(), 0x36fb328ULL};
}

// libr15.so's slot for CSysInfo::OSBuildVersion, defined in libr15 itself (JUMP_SLOT 0x36ff9b8, `readelf -rW` on the
// pinned image: `R_AARCH64_JUMP_SLOT 0xf875c4 _ZN10NRadEngine8CSysInfo14OSBuildVersionERy`).
inline GotTarget LibR15OSBuildVersion() {
  return {kLibR15, kOSBuildVersionSymbol, RelocKind::kJumpSlot, LibR15PatchedBuildId(), 0x36ff9b8ULL};
}

// libr15.so's provider slots (#1220), each defined in libr15 itself (`readelf -rW` on the pinned image):
//   R_AARCH64_JUMP_SLOT 0x36f01a8 -> 0x192f3e4 CNSProvider::Create
//   R_AARCH64_JUMP_SLOT 0x37012d8 -> 0x192f5c0 CNSProvider::Destroy
//   R_AARCH64_JUMP_SLOT 0x36ed158 -> 0x192f27c CNSProvider::ProviderID
//   R_AARCH64_JUMP_SLOT 0x36f1c28 -> 0x192f330 CNSProvider::UserProviderID
inline GotTarget LibR15ProviderCreate() {
  return {kLibR15, kProviderCreateSymbol, RelocKind::kJumpSlot, LibR15PatchedBuildId(), 0x36f01a8ULL};
}
inline GotTarget LibR15ProviderDestroy() {
  return {kLibR15, kProviderDestroySymbol, RelocKind::kJumpSlot, LibR15PatchedBuildId(), 0x37012d8ULL};
}
inline GotTarget LibR15ProviderId() {
  return {kLibR15, kProviderIdSymbol, RelocKind::kJumpSlot, LibR15PatchedBuildId(), 0x36ed158ULL};
}
inline GotTarget LibR15UserProviderId() {
  return {kLibR15, kUserProviderIdSymbol, RelocKind::kJumpSlot, LibR15PatchedBuildId(), 0x36f1c28ULL};
}

// NRadEngine::CCanvasUICS::SetText(SCompactPoolHandle, CSymbol64, char const*), defined in libr15 at 0x139111c (80
// bytes, exported; ReVault 0x149111c): `canvas` (x0) is the CCanvasUICS component, `handle` (w1) the canvas actor's pool
// handle, `item` (x2) the CSymbol64 of the text item and `text` (x3) the C string, copied into the item and never kept; a
// null `text` returns 0. The versions page's SetText2 script nodes reach it through libr15's own PLT stub (0xf50440:
// `adrp x16; ldr x17,[x16,#0x908]; add; br x17`, JUMP_SLOT 0x36ff908) from CSetText2Node::Enter (`bl 0xf50440` at pinned
// 0x2731654; ReVault 0x2831654). Measured on the pinned file: 40 branches to the stub, none to the function, one
// relocation names it.
// `readelf -rW`: `R_AARCH64_JUMP_SLOT 0x139111c _ZN10NRadEngine11CCanvasUICS7SetTextENS_18SCompactPoolHandleENS_9CSymbol64EPKc`.
// The slot is in .got, inside PT_GNU_RELRO, in a RW LOAD (data, never executable): a write needs mprotect(RW) and back to
// R, not PROT_EXEC, so the execmod rule that stops an inline hook (docs/adr/0021) does not apply. The other overload
// (`SAssetElement const&`, JUMP_SLOT 0x36c6a00) is not this slot.
inline constexpr const char* kCanvasSetTextSymbol =
    "_ZN10NRadEngine11CCanvasUICS7SetTextENS_18SCompactPoolHandleENS_9CSymbol64EPKc";
inline constexpr std::uint64_t kCanvasSetTextVaddr = 0x139111c;
// The UI text writer's other two game calls (runtime/patch/ui_text.h; the PC's are in runtime/patch/ui_text.cpp), with the prologue words
// the writer checks before it calls them. SetText is the function above, called directly (the versions-label hook sits on the
// PLT slot, not on the code). The words are the store libr15's; got_pinned_test.cpp compares them with the real file.
//   NRadEngine::CncaGame::GameSpace(unsigned long long) (.dynsym 0x17e17fc, 36 bytes, no relative operand): x0 = the game
//   object (a CR15Game, whose `this` it is), x1 = the index; `index < [x0+0x1f78] ? [[x0+0x1f48] + index*16 + 8] : 0`. The PC's
//   GetGameSpace is the same function at +0x1f50 and +0x1f80. ReVault 0x18e17fc.
inline constexpr std::uint64_t kGameSpaceVaddr = 0x17e17fc;
inline constexpr std::uint32_t kGameSpaceCode[9] = {0xf94fbc08, 0xeb01011f, 0x540000a9, 0xf94fa408, 0x8b011108,
                                                    0xf9400500, 0xd65f03c0, 0xaa1f03e0, 0xd65f03c0};
//   NRadEngine::CLocalComponentHandle<NRadEngine::CCanvasUICS>::Find(CncaGameSpaceMT*, SActor, CSymbol64) (.dynsym 0x1cbb408,
//   344 bytes): x0 = the handle it fills ({CCanvasUICS*, u32 handle}), x1 = the gamespace, x2 = the actor's identity
//   (0x0000ffffffff0000 | index), x3 = the component symbol (canvasui); it leaves x0's canvas null when there is none. The
//   words are the first twelve, up to the ADRP at 0x1cbb438. ReVault 0x1dbb408.
inline constexpr std::uint64_t kCanvasFindVaddr = 0x1cbb408;
inline constexpr std::uint32_t kCanvasFindCode[12] = {0xd10183ff, 0xf90013f8, 0xa9035bf7, 0xa90453f5, 0xa9057bf3, 0xaa0003f4,
                                                      0xa9008be1, 0xb4000901, 0xaa0103f5, 0xf9424821, 0xaa0303f3, 0xb4000761};
//   CCanvasUICS::SetText's first six words (the BL after them is relative and not pinned).
inline constexpr std::uint32_t kCanvasSetTextCode[6] = {0xb4000243, 0xd10083ff, 0xa9017bf3, 0xaa0303f3, 0x92407c21, 0x910023e3};
inline constexpr std::uint64_t kCanvasSetTextStub = 0xf50440;
inline constexpr std::size_t kCanvasSetTextStubCallers = 40;  // b/bl to the stub in the executable segments; none to the function
using CanvasSetTextSig = std::uint64_t(void* canvas, std::uint32_t handle, std::uint64_t item, const char* text);
struct LibR15CanvasSetTextTag {};
using LibR15CanvasSetTextThunk = CallbackThunk<LibR15CanvasSetTextTag, CanvasSetTextSig>;
inline GotTarget LibR15CanvasSetText() {
  return {kLibR15, kCanvasSetTextSymbol, RelocKind::kJumpSlot, LibR15PatchedBuildId(), 0x36ff908ULL};
}

// libr15.so's slot for CR15UIPage2EnablePageNode::Enter, defined in libr15 itself (JUMP_SLOT 0x36c1cf8).
inline GotTarget LibR15EnablePageNodeEnter() {
  return {kLibR15, kEnablePageNodeEnterSymbol, RelocKind::kJumpSlot, LibR15PatchedBuildId(), 0x36c1cf8ULL};
}

// libr15.so's slots for the two NDK storage-manager imports (JUMP_SLOT 0x36c39d0 and 0x36f5b48, readelf -rW
// on the pinned image; both are imported, value 0).
inline GotTarget LibR15MountObb() {
  return {kLibR15, kMountObbSymbol, RelocKind::kJumpSlot, LibR15PatchedBuildId(), 0x36c39d0ULL};
}
inline GotTarget LibR15GetMountedObbPath() {
  return {kLibR15, kGetMountedObbPathSymbol, RelocKind::kJumpSlot, LibR15PatchedBuildId(), 0x36f5b48ULL};
}

// libr15.so's slot for CSysInfo::GetDataRootDir, defined in libr15 itself (JUMP_SLOT 0x36f1df8, `readelf -rW` on the
// pinned image: `R_AARCH64_JUMP_SLOT 0xf87250 _ZN10NRadEngine8CSysInfo14GetDataRootDirEv`).
inline GotTarget LibR15GetDataRootDir() {
  return {kLibR15, kGetDataRootDirSymbol, RelocKind::kJumpSlot, LibR15PatchedBuildId(), 0x36f1df8ULL};
}

// libr15.so's slot for CSysModule::FindMethod, defined in libr15 itself (JUMP_SLOT 0x36db9f8, `readelf -rW` on the
// pinned image).
inline GotTarget LibR15FindMethod() {
  return {kLibR15, kFindMethodSymbol, RelocKind::kJumpSlot, LibR15PatchedBuildId(), 0x36db9f8ULL};
}

// libr15.so's slot for the VrApi device-extension import (JUMP_SLOT 0x36dd440, `readelf -rW` on the pinned image;
// imported, value 0).
inline GotTarget LibR15GetDeviceExtensionsVulkan() {
  return {kLibR15, kGetDeviceExtensionsVulkanSymbol, RelocKind::kJumpSlot, LibR15PatchedBuildId(), 0x36dd440ULL};
}

// libr15.so's slot for vrapi_GetSystemPropertyInt (JUMP_SLOT 0x36fa728, `readelf -rW` on the pinned image; imported,
// value 0).
inline GotTarget LibR15GetSystemPropertyInt() {
  return {kLibR15, kGetSystemPropertyIntSymbol, RelocKind::kJumpSlot, LibR15PatchedBuildId(), 0x36fa728ULL};
}

// libr15.so's slot for CVR::RefreshRTSize, defined in libr15 itself (JUMP_SLOT 0x36dbd10, `readelf -rW` on the pinned image; the
// seven PLT callers of its stub reach it through this slot, the live apply calls it at the vaddr below).
inline GotTarget LibR15RefreshRTSize() {
  return {kLibR15, kRefreshRTSizeSymbol, RelocKind::kJumpSlot, LibR15PatchedBuildId(), 0x36dbd10ULL};
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
  return {kLibR15, kSetDisplayRefreshRateSymbol, RelocKind::kJumpSlot, LibR15PatchedBuildId(), 0x36ec5f0ULL};
}
inline GotTarget LibR15SetClockLevels() {
  return {kLibR15, kSetClockLevelsSymbol, RelocKind::kJumpSlot, LibR15PatchedBuildId(), 0x36c75f0ULL};
}
inline GotTarget LibR15SetPropertyInt() {
  return {kLibR15, kSetPropertyIntSymbol, RelocKind::kJumpSlot, LibR15PatchedBuildId(), 0x36ffcf0ULL};
}
inline GotTarget LibR15GetSystemPropertyFloat() {
  return {kLibR15, kGetSystemPropertyFloatSymbol, RelocKind::kJumpSlot, LibR15PatchedBuildId(), 0x36c06f8ULL};
}
inline GotTarget LibR15SubmitFrame2() {
  return {kLibR15, kSubmitFrame2Symbol, RelocKind::kJumpSlot, LibR15PatchedBuildId(), 0x36e7010ULL};
}

// VrApi_Input.h: ovrResult vrapi_GetCurrentInputState(ovrMobile* ovr, const ovrDeviceID deviceID,
// ovrInputStateHeader* inputState). Imported (value 0): JUMP_SLOT 0x36ec0f0 (`readelf -rW` on the pinned image); its PLT
// stub 0xf29410 (`adrp x16, 0x36ec000; ldr x17, [x16, #0xf0]`) has one caller, CVR::PollTracking at 0x1a71928
// (`revault_callers 0x1029410`; ReVault VA = pinned VA + 0x100000). The input seam of #1202 (input_seam_hook.h).
struct OvrMobileOpaque;
using GetCurrentInputStateSig = int(OvrMobileOpaque* ovr, std::uint32_t device_id, void* input_state);
struct LibR15GetCurrentInputStateTag {};
using LibR15GetCurrentInputStateThunk = CallbackThunk<LibR15GetCurrentInputStateTag, GetCurrentInputStateSig>;

inline GotTarget LibR15GetCurrentInputState() {
  return {kLibR15, kGetCurrentInputStateSymbol, RelocKind::kJumpSlot, LibR15PatchedBuildId(), 0x36ec0f0ULL};
}

// libr15.so's slot for vrapi_GetInputDeviceCapabilities (JUMP_SLOT 0x36fc9c8, `readelf -rW` on the pinned image; imported,
// value 0). Read, never written: the input seam asks a device's hand through the function the game itself calls
// (CVR::PollTracking, 0x1a718fc).
inline GotTarget LibR15GetInputDeviceCapabilities() {
  return {kLibR15, kGetInputDeviceCapabilitiesSymbol, RelocKind::kJumpSlot, LibR15PatchedBuildId(), 0x36fc9c8ULL};
}

// libr15.so's slot for CResourceManager::FillResourceFileRequest, defined in libr15 itself (JUMP_SLOT 0x36efa80,
// `readelf -rW` on the pinned image; the PLT stub at 0xf30730 loads it).
inline GotTarget LibR15FillResourceFileRequest() {
  return {kLibR15, kFillResourceFileRequestSymbol, RelocKind::kJumpSlot, LibR15PatchedBuildId(), 0x36efa80ULL};
}

// libpnsradmatchmaking.so's slot for the same function, defined in
// libpnsradmatchmaking itself at 0x209484 (JUMP_SLOT 0x6b4768).
inline GotTarget MatchmakingTString() {
  return {kMatchmaking, kTStringSymbol, RelocKind::kJumpSlot, kMatchmakingBuildId, 0x6b4768ULL};
}

// libr15.so's slot for SNSConfigRequestv24Send, a weak function it defines at
// 0x191625c (JUMP_SLOT 0x37015e0).
inline GotTarget LibR15ConfigRequestSend() {
  return {kLibR15, kConfigRequestSendSymbol, RelocKind::kJumpSlot, LibR15PatchedBuildId(), 0x37015e0ULL};
}

// libr15.so's slot for CResourceManager::AsyncResourceIOCallback, a GLOB_DAT of libr15's own symbol (0x37195b8): the
// address ResourceLoadInternal hands to ReadFileAsync (#966).
inline GotTarget LibR15AsyncResourceIOCallback() {
  return {kLibR15, kAsyncResourceIOCallbackSymbol, RelocKind::kGlobDat, LibR15PatchedBuildId(), 0x37195b8ULL};
}

// libr15.so's JUMP_SLOT for NR15Logger::MetricLog (#966, ADR 0017): the slot libr15's call to the function loads.
inline GotTarget LibR15MetricLog() {
  return {kLibR15, kMetricLogSymbol, RelocKind::kJumpSlot, LibR15PatchedBuildId(), kMetricLogSlotVaddr};
}

// Two GLOB_DAT slots in libr15.so (0x3715620 and 0x372b698), used as fixtures
// for the GLOB_DAT path.
inline GotTarget LibR15BindNodeGlobDat() {
  return {kLibR15,
          "_ZN10NRadEngine8BindNodeINS_8NRadGame28SR15NetConfigRequestNodeDataENS1_24CR15NetConfigRequestNodeEEExRNS_13SScriptThreadEPv",
          RelocKind::kGlobDat, LibR15PatchedBuildId(), 0x3715620ULL};
}
inline GotTarget LibR15LookupDataBindingGlobDat() {
  return {kLibR15,
          "_ZN10NRadEngine17LookupDataBindingINS_8NRadGame28SR15NetConfigRequestNodeDataEEEvNS_9CSymbol64EPKciPiPPFvPviS7_E",
          RelocKind::kGlobDat, LibR15PatchedBuildId(), 0x372b698ULL};
}

// ---- the Texture Resolution control and its live apply (#1012, docs/adr/0015) -----------------------------------------------
//
// Five functions libr15 defines and calls only through its own PLT (`readelf -rW` on the pinned image: one JUMP_SLOT each;
// design-1012 found no direct `bl` to any of them), plus six it exports that the live apply calls itself. ReVault shows every
// address 0x100000 higher.
//
//   CJson::DecodeFrom(CFileSp const&, unsigned int)       0xfa8120   JUMP_SLOT 0x36ef630. `self` (x0), the file's path as a C string
//                         (x1: the CFileSp is a 512-byte path buffer), the strip-comments flag (w2). Reads the file with
//                         ReadFileIntoString, strips its comments when w2 is set and parses the text with the overload below
//                         (`bl` at 0x10a82e0 ReVault), x1 the text and x2 its length without the terminator. Returns the parse's int.
//   CJson::DecodeFrom(char const*, unsigned long long)    0xfa7e8c   JUMP_SLOT 0x36cda80. `self` (x0), the text (x1), its length (x2).
//                         47 callers, most of them network replies; the control's hook looks at it only inside the file read of the
//                         two documents (texture_menu_hook.cpp).
//   NRadGame::ValidateGfxSettings(SR15GfxSettings&)        0x20e98a4  JUMP_SLOT 0x36e9d48. Clamps the settings in place. Its one clamp
//                         of the msaa field (+0x1c) is `csel w10,w10,w21,lt` at 0x21e9958 ReVault: min(msaa, 2).
//   NRadGame::ApplyRuntimeGfxSettings(SR15GfxSettings const&, CR15Game&, unsigned)   0x20e8e0c  JUMP_SLOT 0x36d9008. Called from
//                         CR15Game::InitEngine at boot and from the settings component on every change. The msaa field (+0x1c) is
//                         read only by a log line that indexes a three-entry name table (kMsaaNames, ELF 0x3690278: three RELATIVE
//                         string pointers, then three ABS64 pointers at 0x3690290, 0x3690298 and 0x36902a0), so an index of 3 to 5
//                         prints through a valid pointer.
//   CEngine::CheckForResize()                              0x2ac0f9c  JUMP_SLOT 0x36d6e10. The one caller is CEngine::RenderBefore
//                         (bl 0x2ac0f4c), once per rendered frame on the thread that draws; x0 is the engine and the result is the
//                         engine's error object, which RenderBefore passes to OutputError. It calls CVR::RefreshRTSize each frame.
//
// Exported and called by the live apply (base + vaddr, after the build ID and the first words are proved, as QuitOnError is):
//   CVR::RefreshRTSize() 0x1a709a0 (writes the eye-size global from vrapi ids 5 and 6, so the existing hook answers it);
//   CEngine::Resize(unsigned, unsigned, EWindowMode) 0x2ac093c (the engine's own resize cycle; the engine's vtable slot +0x30
//     holds it: ABS64 0x36bd7f0), which flushes the GPU, rebuilds the swap chain, the render targets and the pass framebuffers;
//   CVR::Initialized() 0x1a70b64, CVR::GraphicsInitialized() 0x1a71420, CVR::DisplayLost() 0x1a73460;
//   CSysInfo::GetLocalAppDataDirectory(CFixedStringT<512>&) 0xf86d88, which the settings file's path starts from
//     ("%s/RAD/LoneEcho/%s" with "settings_mp_v2_quest.json", libr15 strings).
inline constexpr const char* kJsonDecodeFromFileSymbol = "_ZN10NRadEngine5CJson10DecodeFromERKNS_7CFileSpEj";
inline constexpr const char* kJsonDecodeFromTextSymbol = "_ZN10NRadEngine5CJson10DecodeFromEPKcy";
inline constexpr const char* kValidateGfxSettingsSymbol = "_ZN10NRadEngine8NRadGame19ValidateGfxSettingsERNS0_15SR15GfxSettingsE";
inline constexpr const char* kApplyRuntimeGfxSettingsSymbol =
    "_ZN10NRadEngine8NRadGame23ApplyRuntimeGfxSettingsERKNS0_15SR15GfxSettingsERNS0_8CR15GameEj";
inline constexpr const char* kCheckForResizeSymbol = "_ZN10NRadEngine7CEngine14CheckForResizeEv";

using JsonDecodeFromFileSig = int(CJsonOpaque* self, const char* path, std::uint32_t stripComments);
using JsonDecodeFromTextSig = int(CJsonOpaque* self, const char* text, unsigned long long length);
using ValidateGfxSettingsSig = void(unsigned char* settings);
using ApplyRuntimeGfxSettingsSig = void(const unsigned char* settings, void* game, std::uint32_t refreshUi);
using CheckForResizeSig = std::uint64_t(void* engine);
struct LibR15JsonDecodeFromFileTag {};
struct LibR15JsonDecodeFromTextTag {};
struct LibR15ValidateGfxSettingsTag {};
struct LibR15ApplyRuntimeGfxSettingsTag {};
struct LibR15CheckForResizeTag {};
using LibR15JsonDecodeFromFileThunk = CallbackThunk<LibR15JsonDecodeFromFileTag, JsonDecodeFromFileSig>;
using LibR15JsonDecodeFromTextThunk = CallbackThunk<LibR15JsonDecodeFromTextTag, JsonDecodeFromTextSig>;
using LibR15ValidateGfxSettingsThunk = CallbackThunk<LibR15ValidateGfxSettingsTag, ValidateGfxSettingsSig>;
using LibR15ApplyRuntimeGfxSettingsThunk = CallbackThunk<LibR15ApplyRuntimeGfxSettingsTag, ApplyRuntimeGfxSettingsSig>;
using LibR15CheckForResizeThunk = CallbackThunk<LibR15CheckForResizeTag, CheckForResizeSig>;

inline GotTarget LibR15JsonDecodeFromFile() {
  return {kLibR15, kJsonDecodeFromFileSymbol, RelocKind::kJumpSlot, LibR15PatchedBuildId(), 0x36ef630ULL};
}
inline GotTarget LibR15JsonDecodeFromText() {
  return {kLibR15, kJsonDecodeFromTextSymbol, RelocKind::kJumpSlot, LibR15PatchedBuildId(), 0x36cda80ULL};
}
inline GotTarget LibR15ValidateGfxSettings() {
  return {kLibR15, kValidateGfxSettingsSymbol, RelocKind::kJumpSlot, LibR15PatchedBuildId(), 0x36e9d48ULL};
}
inline GotTarget LibR15ApplyRuntimeGfxSettings() {
  return {kLibR15, kApplyRuntimeGfxSettingsSymbol, RelocKind::kJumpSlot, LibR15PatchedBuildId(), 0x36d9008ULL};
}
inline GotTarget LibR15CheckForResize() {
  return {kLibR15, kCheckForResizeSymbol, RelocKind::kJumpSlot, LibR15PatchedBuildId(), 0x36d6e10ULL};
}

// The functions the live apply calls, with their first words (the store file at each vaddr; the three short ones are 12 or 20
// bytes, so three or four words are all there is to compare) and their signatures. `first` is how many words of `code` are compared.
struct PinnedFunction {
  const char* name;
  std::uint64_t vaddr;
  std::uint32_t code[4];
  std::size_t words;
};
inline constexpr PinnedFunction kRefreshRTSize = {"_ZN10NRadEngine3CVR13RefreshRTSizeEv", 0x1a709a0,
                                                  {0xd10103ff, 0xf90013f4, 0xa9037bf3, 0xaa1f03e0}, 4};
inline constexpr PinnedFunction kEngineResize = {"_ZN10NRadEngine7CEngine6ResizeEjjNS_11EWindowModeE", 0x2ac093c,
                                                 {0x6dbc23e9, 0xf9000bf6, 0xa90253f5, 0xa9037bf3}, 4};
inline constexpr PinnedFunction kVrInitialized = {"_ZN10NRadEngine3CVR11InitializedEv", 0x1a70b64,
                                                  {0xf000e988, 0xf941fd08, 0xf100011f, 0x1a9f07e0}, 4};
inline constexpr PinnedFunction kVrGraphicsInitialized = {"_ZN10NRadEngine3CVR19GraphicsInitializedEv", 0x1a71420,
                                                          {0xd000e988, 0x394d8100, 0xd65f03c0, 0}, 3};
inline constexpr PinnedFunction kVrDisplayLost = {"_ZN10NRadEngine3CVR11DisplayLostEv", 0x1a73460,
                                                  {0x9000e988, 0xb9438900, 0xd65f03c0, 0}, 3};
inline constexpr PinnedFunction kLocalAppDataDirectory = {
    "_ZN10NRadEngine8CSysInfo24GetLocalAppDataDirectoryERNS_13CFixedStringTILy512EEE", 0xf86d88,
    {0xf81d0ff6, 0xa90153f5, 0xa9027bf3, 0xaa0003f3}, 4};
// The reference build's eye-size route (#1737 C4, refresh_eye_size.h): CGRenderConfig::CustomViewportScale (six words; it returns the two
// floats of the viewport scale in s0 and s1), the vrapi PLT stub that RefreshRTSize calls ids 5 and 6 through, and the eye
// globals RefreshRTSize reads and writes. Identical in the stock and the reference file.
struct PinnedCode {
  const char* name;
  std::uint64_t vaddr;
  std::uint32_t code[6];
  std::size_t words;
};
inline constexpr PinnedCode kCustomViewportScale = {"CGRenderConfig::CustomViewportScale", 0x188d0f4ULL,
                                                    {0xf000f868, 0x9101a108, 0x8b000d08, 0xbd411100, 0xbd411501, 0xd65f03c0},
                                                    6};
inline constexpr PinnedCode kVrapiPltStub = {"vrapi_GetSystemPropertyInt PLT stub", 0xf46080ULL,
                                             {0x90013db0, 0xf9439611, 0x911ca210, 0xd61f0220, 0, 0},
                                             4};
namespace eye_layout {
inline constexpr std::uint64_t kStoredPairVaddr = 0x37a33e0ULL;    // {int32 w, int32 h}: RefreshRTSize's result (fcvtzs)
inline constexpr std::uint64_t kOverridePairVaddr = 0x37a33e8ULL;  // {int32 w, int32 h}: SetRTSizeOverride's; non-zero skips the query
inline constexpr std::uint64_t kOvrJavaVaddr = 0x37a3550ULL;       // the ovrJava object the game passes to vrapi
}  // namespace eye_layout

using RefreshRTSizeFn = void (*)();
using EngineResizeFn = int (*)(void* engine, std::uint32_t width, std::uint32_t height, std::int32_t mode);
using VrFlagFn = int (*)();
using LocalAppDataDirectoryFn = void (*)(void* fixedString);

// CEngine fields the live apply reads (the same ones CheckForResize and CEngine::Resize read; design-1012 evidence): the ready
// flag, the engine's resize slot and the display the engine keeps (a CGDisplay at +0x13a68: width +0x3250, height +0x3254, window
// mode +0x32d0), which are the arguments that make CEngine::Resize leave the display itself alone.
namespace engine_layout {
inline constexpr std::size_t kReadyFlags = 0x16ed4;  // byte, bit 0: the engine is ready (CheckForResize, RenderBefore, Resize)
inline constexpr std::uint8_t kReadyBit = 1;
inline constexpr std::size_t kResizeVtableOffset = 0x30;  // *(*engine + 0x30) is CEngine::Resize(w, h, mode)
inline constexpr std::size_t kDisplayWidth = 0x16cb8;
inline constexpr std::size_t kDisplayHeight = 0x16cbc;
inline constexpr std::size_t kDisplayMode = 0x16d38;
}  // namespace engine_layout

// SR15GfxSettings fields (ReVault: ValidateGfxSettings, lane G): the msaa field the control rides on.
namespace gfx_layout {
inline constexpr std::size_t kMsaa = 0x1c;
}

}  // namespace sentinel::pinned
