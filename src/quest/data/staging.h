/* The filesystem side of the first-run data step: staging, the swap, leftovers and the space check
 * (docs/adr/0011-quest-game-data-first-run.md). Plain POSIX, no exceptions.
 *
 * The data is extracted into `<root>/_data.staging-<sha prefix>` and moved into place by one rename to `<root>/_data`,
 * so `_data` is either absent, the old tree, or the complete new one. A `_data` being replaced is renamed to
 * `<root>/_data.replaced-<utc>` first and kept: the step never deletes it.
 *
 * Ownership is the item's manifest: a marker file beside the item, `<item>.nevr-owned`, made before the item exists.
 * Its lines are the item's members, one '/'-separated path relative to the item per line (core/owned_remove_rules.h's
 * entry grammar), written and fsynced before a member is created. A deleter removes only what the manifest names, one
 * entry at a time, through nevr_owned (core/owned_remove.h): RemoveItem never walks a tree, never follows a link, and
 * keeps a folder that still holds a file the manifest does not name. A marker that is a link, a directory, unreadable,
 * over kMaxOwnerManifestBytes or holds a line the grammar refuses is someone else's, and its item is left.
 *
 * Leftovers are found through a ledger, not a listing: every directory the data code writes into has a literal
 * `.nevr-staging-ledger` file, and each item name is appended (fsynced) to its directory's ledger before the item or its
 * marker is made. The root's ledger also names each bundled-pack directory (`_data/<ver>/rad15/android/{manifests,packages}`)
 * before it is created. Cleanup handles exactly the names a ledger lists; an item on disk that no ledger names is not
 * touched. Every rename is followed by an fsync of the directory.
 */
#pragma once

#include <cstddef>
#include <cstdint>
#include <string>
#include <vector>

#include "quest/data/zip.h"

namespace nevr_quest::data {

enum class StageStatus { kOk = 0, kRenameFailed, kStatFailed, kNoSpace, kRemoveFailed, kBadRoot };

const char* StageStatusName(StageStatus status) noexcept;

// `<root>/_data.staging-<first 12 characters of sha256_hex>`
std::string StagingPath(const std::string& root, const std::string& sha256_hex);

// The largest marker (a member list) that is read or written. A bigger one is someone else's.
inline constexpr std::size_t kMaxOwnerManifestBytes = 4u << 20;
// The largest ledger that is read or written.
inline constexpr std::size_t kMaxLedgerBytes = 1u << 20;
// The literal name of the ledger in each directory the data code writes into, and of its temporary during a rewrite.
inline constexpr const char* kLedgerName = ".nevr-staging-ledger";
inline constexpr const char* kLedgerTmpName = ".nevr-staging-ledger.tmp";

// `<item>.nevr-owned`
std::string OwnerMarkerPath(const std::string& item);

// Makes the item's marker beside `item` (the item need not exist yet), with an empty member list, and syncs its directory.
// Call it before the item or its first member is made. The item's name is appended to its directory's ledger first. When a
// marker is already there, it is accepted only if it reads as a valid manifest (an earlier marker of ours: a resume), and it is
// never truncated; anything else at that name (a link, a directory, a file with content the grammar refuses) fails the call.
bool MarkOwned(const std::string& item) noexcept;
// Appends `members` to the manifest of `item`, one fsync for the batch. Each member must pass the entry grammar. Call it only for
// an item this run made with MarkOwned, and before a member is created: a member recorded after it is made is still removed
// when the item is removed, but a crash in between leaves it unrecorded.
bool RecordOwned(const std::string& item, const std::vector<std::string>& members) noexcept;
// Reads the manifest of `item` (creation order) into `members`. A torn tail is tolerated: trailing NUL bytes and a final line with no
// newline are dropped (a kill or a full disk cut the last write short; the record was fsynced before any member was made). False when the
// marker is missing, a link, a directory, over kMaxOwnerManifestBytes, or holds a line the entry grammar refuses.
bool ReadOwnerManifest(const std::string& item, std::vector<std::string>* members) noexcept;
// True when ReadOwnerManifest would succeed for `item`.
bool IsOwned(const std::string& item) noexcept;

// The outcome of RemoveItem. `removed` is true when the item, its members and its marker are all gone. `had_item` is true when
// an item (not only its marker) was there to remove. When `removed` is false, `outcome` is nevr_owned::OutcomeName of the entry
// that stopped it (or "not_owned", "refused_name" or "failed") and `entry` is that entry, relative to the item's directory.
struct ItemRemoval {
  bool removed = false;
  bool had_item = false;
  std::string outcome;
  std::string entry;
};

// Removes one item this module made: its members (children first), then the item folder when empty, then the marker. The item
// is never a link (a link there is refused and left), and nothing that the manifest does not name is removed. An item with no
// valid marker is left as it is. A marker whose item is gone is removed. The build before manifests wrote an empty marker, which
// names no member: for such a folder `legacy_members` (the caller's record, e.g. a zip listing) names the members instead, and the
// folder goes only when those are all it holds.
ItemRemoval RemoveItem(const std::string& item, const std::vector<std::string>* legacy_members = nullptr) noexcept;

// Removes the named members of `item` (children first), then the folder when empty, then the marker when one is there and reads as a
// valid manifest. Nothing is read from a manifest: the caller's record names the members. An item whose marker does not read is
// someone else's, and is left.
ItemRemoval RemoveRecorded(const std::string& item, const std::vector<std::string>& members) noexcept;

// A marker that does not read is moved aside, never truncated: its bytes are renamed to the literal `<marker>.bad` (an earlier `.bad` is
// replaced by that name), and a fresh empty marker is made beside the item (the legacy shape, cleared by the names the caller holds).
// `pinned` names a marker at a name this build itself makes (the pinned staging, the pinned stock cache or partial): then a marker of any
// kind is moved aside. Otherwise only the bytes of a torn record of ours (nothing, NUL bytes only, or one unterminated line that is a
// prefix of a member in `expected`) are; anything else is someone else's, left exactly as it is, and false is returned. A link or a
// directory at the marker's name is refused. True when the marker is absent, valid, or moved aside.
bool ResetUnreadableMarker(const std::string& item, const std::vector<std::string>& expected, bool pinned,
                           bool* truncated = nullptr) noexcept;

// RemoveRecorded for an item whose record may be torn: the marker is reset if it does not read, and the names removed are its readable
// members together with `extra`, the caller's own record of what it made (for the pinned staging, the zip's listing; for a stock cache,
// the pins). Nothing outside those names is removed.
ItemRemoval RemoveUnionRecord(const std::string& item, const std::vector<std::string>& extra) noexcept;

// The members an extraction of a zip's entries makes below kZipPrefix: each entry's path under the prefix and each folder above it,
// in name order. False with `detail` for an entry whose path under the prefix is not a plain relative path.
bool PlanZipMembers(const std::vector<ZipEntry>& entries, std::vector<std::string>* members, std::string* detail) noexcept;

// The staging directory the build before manifests left (its marker names no member) is cleared by the members of the zip it was
// extracted from: `part`, the verified download that build kept until the swap. A staging that has a manifest is removed as
// RemoveItem removes it; nothing else is named. A `part` that does not list is refused with `detail`, and the staging is left.
ItemRemoval ClearLegacyStaging(const std::string& staging, const std::string& part, const ZipLimits& limits,
                               std::string* detail, bool pinned = false) noexcept;

// Records, in `root`'s ledger, the two pack directories under `android_rel` (`_data/<version>/rad15/android`), before the pack makes
// them. Idempotent.
bool RecordPackDirectories(const std::string& root, const std::string& android_rel) noexcept;

// fsync of a directory, so a rename or create in it survives a power loss.
bool SyncDir(const std::string& dir) noexcept;

// Deletes each staging item the root's ledger names (RemoveItem), and then puts a bundled content pack's leftovers right
// (CleanPackLeftovers below). A name the ledger does not list is never touched, not a `_data.replaced-*`, not the download. Returns
// how many items it removed (the ones that were there); `failed` (when not null) gets how many it could not remove, and `left`
// (when not null) gets one "outcome entry" line for each item it left.
int CleanLeftovers(const std::string& root, int* failed, std::vector<std::string>* left = nullptr) noexcept;

// What an interrupted placement of a bundled content pack leaves (docs/adr/0029-quest-bundled-content-pack.md): in each pack
// directory the root's ledger names (`_data/<version>/rad15/android/manifests` and `.../packages`), each `<name>.nevr-pack.tmp`
// the directory's ledger names is removed by RemoveItem. A `.stock` backup is never removed here: its marker is removed when the
// backup is gone. Every other file is left. CleanLeftovers calls it, so the data step's own recovery covers a pack too; the pack
// step calls it directly. Returns how many temporaries it removed; `failed` gets how many it could not, `left` the lines.
int CleanPackLeftovers(const std::string& root, int* failed, std::vector<std::string>* left = nullptr) noexcept;

// The crash between the two renames of InstallStaged leaves no `<root>/_data` and the old tree under a marked
// `<root>/_data.replaced-<utc>`. When `_data` is missing it renames the newest such directory back (and removes its
// marker); a marked name that is not a real directory (a link) is skipped. Returns true when it restored one. Run it before CleanLeftovers and before the data decision.
bool RecoverReplaced(const std::string& root, std::string* restored_from) noexcept;

// Called by InstallStaged at the points where a crash would leave a state the next launch must handle: "before_first_rename"
// (the aside marker is written, nothing is moved) and "between_renames" (the old tree is aside, `_data` is absent). For the
// tests, which look at the directory at that moment; null in production.
struct InstallProbe {
  void (*at)(void* context, const char* point) = nullptr;
  void* context = nullptr;
};

// Moves a complete `staging` directory into place as `<root>/_data`. When `<root>/_data` exists it is first renamed to
// `<root>/_data.replaced-<utc_stamp>`; when that rename fails nothing else is touched. When the final rename fails the
// old tree is moved back.
StageStatus InstallStaged(const std::string& root, const std::string& staging, const std::string& utc_stamp,
                          std::string* replaced_to, const InstallProbe* probe = nullptr);

// Whether the filesystem holding `root` has at least `needed` bytes free for an unprivileged writer.
StageStatus CheckSpace(const std::string& root, std::uint64_t needed, std::uint64_t* available);

}  // namespace nevr_quest::data
