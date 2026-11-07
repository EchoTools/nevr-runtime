// content_pack.h: the runtime serves an installed content pack on PC, client and server (#1424, docs/reference/content-packs.md).
//
// A pack is a folder `<root>\content\<id>\` (nevr_root::ContentDir) with a content.json (content_json.h) and the files it lists:
// manifests that replace stock ones, the packages those add, and script DLLs. Nothing is written in the game folder: the game
// opens its own paths and is handed the pack's files.
//
// - Manifests and packages: the game opens both through one function, CFile::Open (echovr.exe 0x1400e15e0): AddPackage opens
//   each package (0x140fa1264), and DecompressAndReadFile reads each manifest through CFileStream (vtable 0x141d3ed28 slot
//   +0x78, 0x1415220a0, which calls CFile::Open at 0x1415222ac). Both open with flags 1 (read). The one hook here, on that
//   function, hands a read of a stock path the pack replaces the pack's file instead; every other open is passed on unchanged.
// - Scripts: CScriptResource's loader (0x1412ebb50) loads `<exe dir>/scripts/<16 hex>.dll` through the game's module loader,
//   which ends in LoadLibraryW. A load rewriter of the runtime's own LoadLibrary hooks (hook/dll_load_hook.h) maps the pack's
//   file for the scripts the pack carries.
// - Checked before anything is served, the pack is refused whole (one Warning, the game runs on its stock data) when: more than
//   one pack is installed; content.json is missing or malformed (content_json.h); a pack file is missing or not the sha-256
//   content.json lists; a stock manifest is not the one the pack was built from; the game has read its data already (loaded
//   too late); the pack carries scripts and the run is signed-only (-require-signed-plugins), or the game is not echovr.exe
//   631547; or the CFile::Open bytes are not the ones this hook was written against.
// - Each pack file is opened once, hashed through that handle and held open (read sharing only) for the rest of the process, so
//   a file cannot be changed or swapped between the check and the game's read.
// The decision, the per-file checks, the served-path table and the text of every line are content_core.h, which the Quest build
// compiles too (#2312); this file is their Win32 file access and the PC hook's half.
// Installed from PreprocessCommandLineHook (lifecycle/boot.cpp) after the early plugin pass and before the game's first
// original call, inside which InitGlobals reads the manifests and opens the packages.
#pragma once

#include <cstddef>
#include <cstdint>
#include <string>

#include "runtime/content/content_core.h"

#ifdef _WIN32
#include <winsock2.h>
#include <windows.h>
#endif

namespace nevr_content {

// What Decide is given: where the packs and the game live, and the facts about this run it cannot read itself.
struct Inputs {
  std::wstring content_dir;  // <root>\content
  std::wstring game_dir;     // the game folder (the one that holds _data)
  std::wstring exe_dir;      // the folder of echovr.exe (bin\win10), which holds the scripts folder
  bool signed_only = false;          // the run is -require-signed-plugins
  bool game_supported = true;        // echovr.exe is the 631547 build
  bool data_read_already = false;    // the game has created its resource manager (and so read its manifests)
};

#ifdef _WIN32
// Checks the installed pack (the shared decision, content_core.h, over Win32 file access) and, when it passes, makes it the one
// this process serves (ServeOpen, RewriteScript). Every file that passed is held open for the rest of the process. Logs nothing:
// Install logs the decision.
Decision Decide(const Inputs& inputs);

// The sha-256 of the whole of `file`, from its start, as 64 lowercase hex characters, read `chunk_bytes` at a time (one buffer of
// that size, whatever the file's size); "" when the file does not read. A pack file is hashed this way through the handle that is
// then held, kDigestChunkBytes at a time.
inline constexpr std::size_t kDigestChunkBytes = 1u << 20;
std::string StreamSha256(HANDLE file, std::size_t chunk_bytes);

// The pack's file for an open of `game_path` (UTF-8, as the game passes it to CFile::Open) with the game's `flags`, as UTF-8 in
// `pack_path`. False for any open the pack does not serve: a path it does not replace, or flags other than a read.
bool ServeOpen(const char* game_path, std::uint32_t flags, std::string* pack_path);

// The DllLoadHook rewriter: the pack's file for a load of `<exe dir>\scripts\<name>.dll` the pack carries.
bool RewriteScript(const wchar_t* lower_path, std::wstring* load_path);

// CFile::Open's type, and the body of its detour: an open the pack serves goes to `original` with the pack's file and is logged
// with the game's result; every other open goes to `original` unchanged.
using CFileOpenFn = std::uint64_t (*)(const char* path, std::uint32_t flags, HANDLE* out);
std::uint64_t OpenThrough(CFileOpenFn original, const char* path, std::uint32_t flags, HANDLE* out);

// Forgets the pack Decide made current and closes the files it held (a pack refused after Decide, and the tests).
void Forget();

// Production (content_pack_install.cpp): decides from this process (the nevr root, the game's folders, the run's mode, the game
// image), logs the decision, and installs the CFile::Open hook and the script rewriter for a pack that passed. Runs once; a
// second call does nothing.
void Install(std::uintptr_t game_base);
#endif

}  // namespace nevr_content
