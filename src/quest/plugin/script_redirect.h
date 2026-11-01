// The script redirect (#1737): a combat unit ships its own build of a stock script library under a new name, and the game's
// load of the stock name opens that build instead. The stock lib<hash>.so files are never replaced and never read in the
// redirected place.
//
// The seam is the libr15 dlopen import (dlopen_hook.cpp), GOT slot 0x36c6380 in the pinned build (revault 0x37c6380).
// LoadPackage formats the script name as lib%016llx.%s (libr15 0x29cf8d0), CSysModule::Load (0x2b9e16c) normalises it
// (the converter at 0x2b9e2d0 appends .so when absent, strips a leading '/', and inserts a constant when the name lacks
// the lib prefix; revault decompile), and Load calls dlopen with the result. Every
// dlopen call in libr15 reaches the one dlopen import through that slot (six call sites, revault-checked), so a rewrite
// there covers every script load, including Load's own, and needs no code written into libr15. Hooking Load would see the
// name before the converter and would miss the other five call sites.
//
// Registry: one registration per process, from the combat unit's init (NevrQuestHostApi::register_script_redirect). It
// is all or nothing: each lib<hash16>_q15t.so must be a file in the directory the sentinel itself was loaded from (the APK's
// native library directory), or nothing is registered. A process with no registration (a plain APK) passes every name
// through unchanged.
//
// This header and script_redirect.cpp are reached from the dlopen handler, so they are built without exceptions and the
// handler's path calls only RewriteLoadName and the sentinel log.
#pragma once

#include <cstddef>
#include <cstdint>

#include "nevr/plugin_interface.h"

namespace nevr_quest::script_redirect {

inline constexpr std::uint32_t kMaxEntries = 16;
// lib<16 hex>_q15t.so is 27 bytes; the buffer the handler uses is this size.
inline constexpr std::size_t kNameCapacity = 64;

// The name the redirected library is built under: lib<hash16 lowercase hex>_q15t.so, into out[cap]. False when it does not
// fit.
bool RedirectName(std::uint64_t hash, char* out, std::size_t cap) noexcept;

// True and the stock hash when the final path component of `name` is lib<hash16>.so or <hash16>.so, with the hash in
// lowercase hex. Any other name (a different prefix or suffix, uppercase hex, a directory part that is not the last '/')
// is false.
bool ParseScriptName(const char* name, std::uint64_t* hash) noexcept;

// The redirect for `name` as the game asks for it. Returns `name` itself unless the registry holds the stock hash of a
// script name; then returns `buf`, which holds the redirected bare name. Every script-library name (a name ParseScriptName
// accepts) logs one Info line, `script_load`, with its name and whether the registry was set; a redirected one also logs
// `script_redirected`. Called from the dlopen handler: no allocation, no exceptions.
const char* RewriteLoadName(const char* name, char* buf, std::size_t cap) noexcept;

// The game's dlopen with the redirect applied: `loader` is the loader's dlopen (or a test double). The name it opens is
// RewriteLoadName's answer. A redirected open that returns null logs `script_redirect_failed` with the name, the redirected
// name and dlerror() text, and returns that null; errno is left as the open set it. Called from the dlopen handler.
void* OpenRedirected(void* (*loader)(const char* name, int flags), const char* name, int flags, char* buf,
                     std::size_t cap) noexcept;

// The registration, against an explicit directory (the tests use a temporary one). `pluginId` names the caller in the log.
NevrScriptRedirectResult RegisterIn(const char* libraryDir, const char* pluginId, const std::uint64_t* hashes,
                                    std::uint32_t count) noexcept;

// The directory of the library at `path` (dladdr's dli_fname), into dir[cap]: null on success, else the reason no directory
// can be checked (a library mapped out of an APK, a path with no directory part, a path too long for dir).
const char* LibraryDirectoryOf(const char* path, char* dir, std::size_t cap) noexcept;

// The registration against the directory of `libraryPath` (dladdr's dli_fname for the sentinel). A path mapped out of an APK
// is refused by name (LibraryDirectoryOf).
NevrScriptRedirectResult RegisterFromLibraryPath(const char* pluginId, const char* libraryPath, const std::uint64_t* hashes,
                                                 std::uint32_t count) noexcept;

// The registration against the directory the sentinel was loaded from (dladdr on this function). Called from plugin init.
// A refusal other than ALREADY_ARMED leaves nothing registered, so a caller may call again.
//
// Timing: the q15t plugin calls this from its NvrPluginInitEx, which the loader runs after a hooked dlopen has returned a
// handle (post_load.cpp, AfterDlopen -> RunPlugins). A script-library load whose hooked open comes before that is not
// redirected; its script_load line (registry_set=false) shows it.
NevrScriptRedirectResult Register(const char* pluginId, const std::uint64_t* hashes, std::uint32_t count) noexcept;

// Logs the redirect state once: `registry=<n>` and `status=armed` or `status=pass-through`. Called when the dlopen hook is
// installed, so a run without a registration still says so (silence is not success).
void LogState() noexcept;

// The number of names in force (0 when nothing is registered).
std::uint32_t ArmedCount() noexcept;

// The log's name for a result, as the host writes it.
const char* ResultName(NevrScriptRedirectResult result) noexcept;

// Clears the registration. Tests only.
void ResetForTest() noexcept;

}  // namespace nevr_quest::script_redirect
