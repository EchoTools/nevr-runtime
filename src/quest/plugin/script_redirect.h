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
// Registry: one registration per process: the combat area's normal step (combat/script_redirect.h, owner `combat`), or a plugin's
// init through NevrQuestHostApi::register_script_redirect. It
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
// script name; then returns `buf`, which holds the redirected bare name. A redirect logs one Info line, `script_redirected`
// (from, to, hash). A registered name that does not fit `buf` is not redirected and logs a Warning, `script_redirect_skipped`.
// A script-library name that is not redirected logs `script_load` (name, whether a registry is set) only when
// SetLogEveryLoad is on: that is per-item detail, and a process loads many script libraries. A name that is not a script
// library logs nothing. Called from the dlopen handler: no allocation, no exceptions.
const char* RewriteLoadName(const char* name, char* buf, std::size_t cap) noexcept;

// The game's dlopen with the redirect applied: `loader` is the loader's dlopen (or a test double). The name it opens is
// RewriteLoadName's answer. A redirected open that returns null logs `script_redirect_failed` with the name, the redirected
// name and dlerror() text, and returns null; errno is left as that open set it.
//
// dlerror(): logging the failure reads, and so clears, the loader's error text. The redirected open is therefore issued a
// second time (never the stock name, which would load the library the redirect replaces), so the game's next dlerror()
// holds the loader's text for the redirected name. A second open that succeeds is returned as the game's handle, with a
// Warning, `script_redirect_recovered`. A failing stock-name open is not touched: its dlerror() is the game's. Called from
// the dlopen handler.
void* OpenRedirected(void* (*loader)(const char* name, int flags), const char* name, int flags, char* buf,
                     std::size_t cap) noexcept;

// The registration, against an explicit directory (the tests use a temporary one). `pluginId` names the caller in the log.
NevrScriptRedirectResult RegisterIn(const char* libraryDir, const char* pluginId, const std::uint64_t* hashes,
                                    std::uint32_t count) noexcept;

// The directory of the library at `path` (dladdr's dli_fname), into dir[cap]: null on success, else the reason no directory
// can be checked, as a stable token (it is the log's `reason`): library_path_missing (no path or no buffer),
// library_inside_apk (mapped out of an APK), library_path_has_no_directory, library_path_too_long (does not fit dir).
const char* LibraryDirectoryOf(const char* path, char* dir, std::size_t cap) noexcept;

// The registration against the directory of `libraryPath` (dladdr's dli_fname for the sentinel). A path mapped out of an APK
// is refused by name (LibraryDirectoryOf).
NevrScriptRedirectResult RegisterFromLibraryPath(const char* pluginId, const char* libraryPath, const std::uint64_t* hashes,
                                                 std::uint32_t count) noexcept;

// The registration against the directory the sentinel was loaded from (dladdr on this function). Called from the combat area's
// normal step, or from a plugin's init. A refusal other than ALREADY_ARMED leaves nothing registered, so a caller may call again.
//
// Timing: the combat area calls this from its normal install step, which runs after a hooked dlopen has returned a handle and the
// plugin pass has returned (post_load.cpp, AfterDlopen -> RunPluginsAndCombat). A script-library load whose hooked open comes
// before that is not redirected; its script_load line (registry_set=false) shows it.
NevrScriptRedirectResult Register(const char* pluginId, const std::uint64_t* hashes, std::uint32_t count) noexcept;

// Per-item script_load lines for loads that are not redirected: on in a development build, off otherwise. Set when the plugin
// runtime is planned (plugin_runtime.cpp), which is before the dlopen hook goes in.
void SetLogEveryLoad(bool on) noexcept;
bool LogsEveryLoad() noexcept;

// Logs the redirect state once: `registry=<n>` and `status=armed` or `status=pass-through`. Called when the dlopen hook is
// installed, so a run without a registration still says so (silence is not success).
void LogState() noexcept;

// The number of names in force (0 when nothing is registered).
std::uint32_t ArmedCount() noexcept;

// The log's name for a result, as the host writes it.
const char* ResultName(NevrScriptRedirectResult result) noexcept;

// Tests only. `hook` runs inside RegisterIn after its argument checks and before it claims the registration: a test makes a
// second registration finish there and shows the first one then refuses (the claim, not an earlier look, decides).
void SetBeforeClaimHookForTest(void (*hook)() noexcept) noexcept;

// Tests only. Makes the registration claim look held (true) or free (false), as if another registration were still deciding:
// a test then shows RegisterIn refuses on the claim itself, before it looks at the registry.
void SetClaimHeldForTest(bool held) noexcept;

// Clears the registration, the per-load logging switch and the claim hook. Tests only.
void ResetForTest() noexcept;

// Map scripts, route B (CDN_DESIGN Q3, docs/adr/0047). A map's script library ships inside the APK (the repack puts
// lib<hash16>.so in lib/arm64-v8a); the channel's package pins its sha-256 in scripts[]. Register checks the packaged file
// against that pin and the ELF rules (core/script_image_rules.h CheckElfScript) and then holds the hash for the map. The
// game's own bare dlopen("lib<hash16>.so") already finds the packaged file, so a held map script is opened under the name the
// game asked for: the table only answers Holds (the set gate's waiver) and records which held scripts the game has loaded.
//
// This table is separate from the one-per-process stock redirect above, which it never changes: a hash that registration
// holds is refused here, and a load the redirect rewrites is not a map-script load. With nothing held, the dlopen handler's
// extra work is one name parse.
//
// Entries are append-only (a withdrawn entry keeps its slot), so the dlopen handler reads them without a lock and never sees a
// slot reused. kMaxMapScripts therefore counts every registration of the process, withdrawn ones included.
namespace map_scripts {

inline constexpr std::uint32_t kMaxMapScripts = 256;
inline constexpr std::size_t kOwnerCapacity = 64;  // "map:" plus an id of at most 55 characters fits with its NUL

// Admit's refusal tokens: bad_owner (null, empty or too long), bad_hash (0), table_full, duplicate (another live entry holds
// the hash), redirected (the stock redirect holds it), withdrawn (the owner was withdrawn this process).
// nullptr when admitted. Called by Register after the file checks; tests call it directly.
const char* Admit(const char* owner, std::uint64_t hash) noexcept;

// Drops every script `owner` holds and returns how many. Refuses (returns 0, logs map_script refused=withdraw_after_load) when
// the game has loaded one of them: a library the game loaded is never pulled. A load that races the withdraw keeps that one
// entry held (logged withdraw_raced_load). The owner is then barred from registering again.
std::size_t Withdraw(const char* owner) noexcept;

// True when `owner` holds `hash` and it is not withdrawn.
bool Holds(const char* owner, std::uint64_t hash) noexcept;

// Live (held or loaded) entries.
std::uint32_t HeldCount() noexcept;

// From the dlopen handler, before the open: when `name` is a held map script (and not a name the stock redirect rewrites),
// records that the game loads it and logs map_script_served once per entry. No allocation, no lock.
//
// Route A (Q4, gated on the headset probe P3) would open the held cache file here instead, through android_dlopen_ext with
// ANDROID_DLEXT_USE_LIBRARY_FD. It is not built: route B's packaged file needs no rewrite.
void NoteLoad(const char* name) noexcept;

// Register (map_scripts.cpp, built apart so the stock redirect's host tests link without the hash and ELF code): `owner` is
// map:<id>, `sha256` the package's 64-lowercase-hex pin for scripts/lib<hash16>.so. Checks, in order: bad_owner, bad_sha256,
// bad_hash, no_library_dir (the sentinel's directory, as the stock registration finds it), missing (no regular file),
// unreadable, bad_size (0 or over 64 MiB), sha_mismatch, an elf_* token, then Admit's tokens. One map_script line per call.
// Returns nullptr when held, else the token.
const char* Register(const char* owner, std::uint64_t hash, const char* sha256);
const char* RegisterIn(const char* libraryDir, const char* owner, std::uint64_t hash, const char* sha256);

// Clears the table. Tests only.
void ResetForTest() noexcept;

}  // namespace map_scripts

}  // namespace nevr_quest::script_redirect
