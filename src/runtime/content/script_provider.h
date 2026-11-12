// script_provider.h: the script DLLs a subscribed map carries, served to the game on PC (CDN_PC PC6).
//
// The game loads a script as `<exe dir>\scripts\<hash16>.dll` (CScriptResource Load, echovr.exe 0x1412ebb50) through
// LoadLibraryW. A map's script is a file in the maps cache that the maps channel opened and holds; Register checks it
// (core/script_image_rules.h) and remembers it under its hash, and the "map-scripts" load rewriter
// (map_scripts_install.cpp) hands the loader that held file for a load of the canonical path. A rekeyed script is the
// player's own stock DLL served under a new hash: no stock bytes ever travel.
//
// - A hash is refused when the game has its own file for it (stock_hash), the content pack carries it (pack_hash), or a
//   map took it first (duplicate); so the content pack's scripts and stock scripts are served exactly as without maps.
// - Refused whole while the run is signed-only (-require-signed-plugins; ADR 0047 decides map scripts there), and on a
//   dedicated server for a map that maps.server_allow does not name (server_not_allowed).
// - A revoked sha-256 (the manifest's revocation list) is dropped from the held set, and refused after.
// - Every held file is held for the process (read sharing only), so a reload after the engine frees the DLL (vslot 6)
//   maps the same, unchanged file.
// One [NEVR.MAPS] line per Register, RegisterRekey and revocation drop; map_scripts_install.cpp logs each serve.
#pragma once

#include <cstdint>
#include <string>
#include <vector>

namespace nevr_map_scripts {

// The largest script file Register reads: stock script DLLs are tens of KiB.
inline constexpr std::uint64_t kMaxScriptBytes = 64ull * 1024 * 1024;

struct Outcome {
  std::string reason = "ok";  // "ok", or a refusal token (script_provider.h's list, or a pe_* token from CheckPeScript)
  std::string detail;
  bool ok() const { return reason == "ok"; }
};

struct Inputs {
  std::wstring exe_dir;                   // the folder of echovr.exe (bin\win10), which holds the scripts folder
  bool signed_only = false;               // the run is -require-signed-plugins
  bool server = false;                    // the run is a dedicated server
  std::vector<std::string> server_allow;  // maps.server_allow: the map ids a server serves scripts for
  // True when the content pack serves the script `lower_path` names (nevr_content::CarriesScript); null: no pack.
  bool (*pack_carries)(const wchar_t* lower_path) = nullptr;
};

// The run's facts. Called once, before the first Register; a second call replaces them (the tests).
void Configure(const Inputs& inputs);

// The manifest's revoked sha-256 list: every held script with one of these digests is dropped (its handle closed), and a
// later Register or RegisterRekey of one is refused (revoked). Called before the mount registers anything.
void SetRevoked(const std::vector<std::string>& revoked_sha256);

// A map's script: `held_handle` is the file, open for reading with read sharing only, and `sha256` its manifest digest
// (64 lowercase hex). `owner` is the map's resource-set owner, "map:<id>". The bytes are read through the handle and
// checked (CheckPeScript); with `rehash` their sha-256 must be `sha256`. On ok() the provider owns the handle for the
// process; on a refusal the caller keeps it. Tokens, checked in this order: not_configured, bad_owner, bad_sha256,
// withdrawn, signed_only, server_not_allowed, revoked, duplicate, stock_hash, pack_hash, unreadable, bad_size,
// sha_mismatch, a pe_* token, no_final_path.
Outcome Register(const std::string& owner, std::uint64_t hash, std::int64_t held_handle, const std::string& sha256,
                 bool rehash);

// A rekeyed script: `hash` is served from the player's own `<exe dir>\scripts\<stock_source16>.dll`, which is opened, held
// and hashed here and must be `stock_sha256`. Tokens as Register, with stock_missing and stock_sha_mismatch in place of
// unreadable and sha_mismatch.
Outcome RegisterRekey(const std::string& owner, std::uint64_t hash, std::uint64_t stock_source,
                      const std::string& stock_sha256);

// A rekeyed script that PC10 rebuilt from the player's own stock script `stock_sha256` (CDN_DESIGN §5.4): the file is
// `<version dir>\scripts\<hash16>.dll` in the maps cache, held through `held_handle` as Register holds a map's script.
// It is checked exactly as a downloaded script (every Register rule, the PE rules included) and always rehashed: its
// bytes must be `sha256`, the recipe's result digest. Tokens as Register (bad_sha256 also for `stock_sha256`), then
// derived_path when the held file is not named scripts\<hash16>.dll. Withdraw drops it with the owner's other scripts.
Outcome RegisterDerived(const std::string& owner, std::uint64_t hash, std::int64_t held_handle, const std::string& sha256,
                        const std::string& stock_sha256);

// True when `owner` registered the script `name`: the resource-set registry's script waiver (SetScriptWaiver).
bool Holds(const std::string& owner, std::uint64_t name);

// The rewriter's answer: the held file's own path for a load of `<exe dir>\scripts\<hash16>.dll` (any case, either
// slash) of a held hash. False for every other path. `owner` and `hash` name what was served, for the log.
bool Rewrite(const wchar_t* lower_path, std::wstring* load_path, std::string* owner, std::uint64_t* hash);

// `<exe dir>\scripts\<hash16>.dll` in the one spelling the rewriter compares: full, backslashes, ASCII lower case.
std::wstring CanonicalScriptPath(const std::wstring& exe_dir, std::uint64_t hash);

// Drops every script and rekey `owner` registered and closes their handles, so none is served again, and refuses
// `owner`'s later registrations (withdrawn). The maps channel calls it for a map whose set did not mount: all or
// nothing. Returns how many were dropped. Refuses, returning 0 and dropping nothing (reason=withdraw_after_load), when
// the rewriter has already served one of `owner`'s scripts: a DLL the game loaded is never pulled. At boot and at the
// mount no map script has loaded yet.
std::size_t Withdraw(const std::string& owner);

// How many scripts are held.
std::size_t HeldCount();

// Drops every held script and closes its handle, and the run's facts and revocations (the tests).
void Forget();

// Production (map_scripts_install.cpp). SetPolicy: Configure from this process (the exe folder, the content pack's
// script set) and the maps channel's facts, and hand Holds to the resource-set script waiver. Called once by the
// maps channel before any Register; a later call replaces the facts.
void SetPolicy(bool is_server, std::vector<std::string> server_allow_ids, bool signed_only);

// Registers the "map-scripts" load rewriter, once; true when it is registered. The maps channel calls it after the
// first Register that is ok, never at static init or from boot, so a run with no map script adds no rewriter. It comes
// after nevr_content::Install's, so the content pack's rewriter stays first and keeps every load of its own scripts.
bool InstallMapScriptsRewriter();

}  // namespace nevr_map_scripts
