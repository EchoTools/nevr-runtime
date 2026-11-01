// The script redirect (#1737): a combat unit ships its own build of a stock script library under a new name, and the game's
// load of the stock name opens that build instead. The stock lib<hash>.so files are never replaced and never read in the
// redirected place.
//
// The seam is the libr15 dlopen import (dlopen_hook.cpp). The game's CSysModule::Load turns its script name into
// lib<hash16>.so (the converter in libr15 0x2b9e2d0 leaves a bare lib...so name alone), then calls dlopen with it. Every
// load of a script library passes through that one slot, and nothing else in libr15 calls dlopen, so the rewrite there
// covers every load and needs no code written into libr15.
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
// script name; then returns `buf`, which holds the redirected bare name, and logs one Info line. Called from the dlopen
// handler: no allocation, no exceptions.
const char* RewriteLoadName(const char* name, char* buf, std::size_t cap) noexcept;

// The registration, against an explicit directory (the tests use a temporary one). `pluginId` names the caller in the log.
NevrScriptRedirectResult RegisterIn(const char* libraryDir, const char* pluginId, const std::uint64_t* hashes,
                                    std::uint32_t count) noexcept;

// The registration against the directory the sentinel was loaded from (dladdr on this function). Called from plugin init.
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
