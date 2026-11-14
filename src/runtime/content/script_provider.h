// script_provider.h: the script DLLs a map carries, served to the game on PC (docs/adr/0048 decision 6).
//
// The game loads a script as `<exe dir>\scripts\<hash16>.dll` (CScriptResource Load, echovr.exe 0x1412ebb50) through
// LoadLibraryW. A map's script is a file in the maps cache that the maps channel opened and holds; Register checks it
// (core/script_image_rules.h) and remembers it under its hash, and the "map-scripts" load rewriter
// (map_scripts_install.cpp) hands the loader that held file for a load of the canonical path. A rekeyed script is the
// player's own stock DLL served under a new hash: no stock bytes ever travel.
//
// - The claim's refusals are the shared rules (core/map_script_check.h): a hash is refused when the game has its own file
//   for it (stock_hash), the content pack carries it (pack_hash), or a map took it first (duplicate); so the content pack's
//   scripts and stock scripts are served exactly as without maps.
// - Under -require-signed-plugins a script is refused (signed_only) unless the manifest that pins it is official: the
//   channel passes that fact per claim, from the same signature check core map_run uses (ADR 0048 decision 7). There is no
//   allow list on any run (decision 18): a dedicated server serves the scripts of the maps its channel mounts.
// - The file's size is bounded by the device's share for a document (MapDocumentBytes, the channel's Inputs::byte_limit) and,
//   when the package index pins it, by the pinned size.
// - A revoked sha-256 (the manifest's revocation list) is dropped from the held set, and refused after.
// - Every held file is held for the process (read sharing only), so a reload after the engine frees the DLL (vslot 6)
//   maps the same, unchanged file.
// One [NEVR.MAPS] line per Register, RegisterRekey and revocation drop; map_scripts_install.cpp logs each serve.
#pragma once

#include <cstdint>
#include <string>
#include <vector>

namespace nevr_map_scripts {

struct Outcome {
  std::string reason = "ok";  // "ok", or a refusal token (the list below, or an image rule's token from core/script_image_rules.h)
  std::string detail;
  bool ok() const { return reason == "ok"; }
};

struct Inputs {
  std::wstring exe_dir;      // the folder of echovr.exe (bin\win10), which holds the scripts folder
  bool signed_only = false;  // the run is -require-signed-plugins
  // The largest file a script may be: the device's share for a document (core/map_manifest.h MapDocumentBytes). Zero means the
  // channel gave no device fact, and every script is refused not_configured.
  std::uint64_t byte_limit = 0;
  // True when the content pack serves the script `lower_path` names (nevr_content::CarriesScript); null: no pack.
  bool (*pack_carries)(const wchar_t* lower_path) = nullptr;
};

// The run's facts. Called once, before the first Register; a second call replaces them (the tests).
void Configure(const Inputs& inputs);

// The manifest's revoked sha-256 list: every held script with one of these digests is dropped (its handle closed), and a
// later Register or RegisterRekey of one is refused (revoked). Called before the mount registers anything.
void SetRevoked(const std::vector<std::string>& revoked_sha256);

// A map's script: `held_handle` is the file, open for reading with read sharing only, and `sha256` its manifest digest
// (64 lowercase hex). `owner` is the map's resource-set owner, "map:<id>". The bytes are read through the handle and checked
// (the size bound, then CheckPeScript); with `rehash` their sha-256 must be `sha256`. `pinned_bytes` is the package index's
// size of the member (0 when nothing pins it). `official` is true when the manifest that pins the script is signed by a
// compiled-in official key. On ok() the provider owns the handle for the process; on a refusal the caller keeps it. Tokens,
// checked in this order: not_configured, bad_owner, bad_hash, bad_sha256, withdrawn, signed_only, revoked, duplicate,
// stock_hash, pack_hash, unreadable, bad_size, sha_mismatch, a rule token, no_final_path.
Outcome Register(const std::string& owner, std::uint64_t hash, std::int64_t held_handle, const std::string& sha256,
                 bool rehash, std::uint64_t pinned_bytes = 0, bool official = false);

// A rekeyed script: `hash` is served from the player's own `<exe dir>\scripts\<stock_source16>.dll`, which is opened, held
// and hashed here and must be `stock_sha256`. The stock bytes pass the same size bound and PE rules as a downloaded script.
// Tokens as Register, with stock_missing and stock_sha_mismatch in place of unreadable and sha_mismatch.
Outcome RegisterRekey(const std::string& owner, std::uint64_t hash, std::uint64_t stock_source,
                      const std::string& stock_sha256, bool official = false);

// A rekeyed script the recipe rebuild wrote (core/map_derive.h) from the player's own stock script `stock_sha256`: the file is
// `<version dir>\scripts\<hash16>.dll` in the maps cache, held through `held_handle` as Register holds a map's script.
// It is checked exactly as a downloaded script (every Register rule, the PE rules included) and always rehashed: its
// bytes must be `sha256`, the recipe's result digest. Tokens as Register (bad_sha256 also for `stock_sha256`), then
// derived_path when the held file is not named scripts\<hash16>.dll. Withdraw drops it with the owner's other scripts.
Outcome RegisterDerived(const std::string& owner, std::uint64_t hash, std::int64_t held_handle, const std::string& sha256,
                        const std::string& stock_sha256, bool official = false);

// True when `owner` registered the script `name`: the resource-set registry's script waiver (SetScriptWaiver).
bool Holds(const std::string& owner, std::uint64_t name);

// The rewriter's answer: the held file's own path for a load of `<exe dir>\scripts\<hash16>.dll` (any case, either
// slash) of a held hash. False for every other path. `owner` and `hash` name what was served, for the log; `first` is set
// to true for the first serve of the script (the rewriter logs that one at Info and the rest at Debug).
bool Rewrite(const wchar_t* lower_path, std::wstring* load_path, std::string* owner, std::uint64_t* hash,
             bool* first = nullptr);

// `<exe dir>\scripts\<hash16>.dll` in the one spelling the rewriter compares: full, backslashes, ASCII lower case.
std::wstring CanonicalScriptPath(const std::wstring& exe_dir, std::uint64_t hash);

// Drops every script and rekey `owner` registered and closes their handles, so none is served again, and refuses
// `owner`'s later registrations (withdrawn), whether or not it held any. The maps channel calls it for a map whose set did
// not mount: all or nothing. Returns how many were dropped. Refuses, returning 0 and dropping nothing
// (reason=withdraw_after_load), when the rewriter has already served one of `owner`'s scripts: a DLL the game loaded is never
// pulled. At boot and at the mount no map script has loaded yet.
std::size_t Withdraw(const std::string& owner);

// How many scripts are held.
std::size_t HeldCount();

// Drops every held script and closes its handle, and the run's facts and revocations (the tests).
void Forget();

// Production (map_scripts_install.cpp). SetPolicy: Configure from this process (the exe folder, the content pack's script
// set) with the run's signed-only mode and the device's byte limit, and hand Holds to the resource-set script waiver. Called
// once by the maps channel before any Register; a later call replaces the facts.
void SetPolicy(bool signed_only, std::uint64_t byte_limit);

// Registers the "map-scripts" load rewriter, once; true when it is registered. The maps channel calls it after the
// first Register that is ok, never at static init or from boot, so a run with no map script adds no rewriter. It comes
// after nevr_content::Install's, so the content pack's rewriter stays first and keeps every load of its own scripts.
bool InstallMapScriptsRewriter();

}  // namespace nevr_map_scripts
