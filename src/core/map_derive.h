/* Recipes (docs/adr/0047-custom-maps-channel.md, the recipe format in core/map_format.h): a map never carries stock bytes, so a
 * stock asset it duplicated or re-keyed arrives as edits against the player's own install. This rebuilds each one from a
 * StockSource, checks it against the recipe's sha256, and writes the merged set the mount serves in place of the published one.
 *
 * Pure over nevr_cargo::Env, MapFs and StockSource: no windows.h and no Log(), so a test drives every rule with fakes. Derive runs
 * on the fetch's worker; StockCurrent wherever the mount runs, where it only re-stamps files.
 *
 * Files (core/map_format.h): the outputs are written under <version dir>/derived.tmp, the merged set appended through
 * Env::OpenAppend and synced, each rebuilt script written whole (Env::WriteFileAtomically); when every output verified the
 * directory is renamed to <version dir>/derived (MapFs::RenameDir). A refusal removes derived.tmp, so it leaves no file.
 *
 * Memory (docs/adr/0045-cdn-texture-delivery.md decision 8): what a rebuild holds at once (the stock bytes, the authored bytes and
 * the result of one recipe; one script image; a set's tables) is checked against a share of the memory the device reports
 * (Env::QueryDevice) before it is allocated. The recipe format's own caps (kRecipeResultMax, kRecipeMapInsertMax) still apply.
 *
 * Reason tokens, "" on success: derived_stock_missing, derived_stock_mismatch, recipe_result, recipe_bounds, recipe_cap,
 * recipe_tiny, rekey_site, derived_dup (a derived key that the set or another recipe already holds), set_format (the published
 * set's tables do not read), file_offset (the merged set outgrows the u32 chunk offsets), no_memory (the device's memory share
 * is smaller than what the rebuild must hold), no_memory_fact (the device gave no memory figure), derive_io (a cache read or
 * write failed) and cancelled (keep_going turned false).
 */
#pragma once

#include <cstddef>
#include <cstdint>
#include <functional>
#include <string>
#include <vector>

#include "core/cargo_env.h"
#include "core/map_types.h"

namespace nevr_maps {

class StockSource {  // read-only view of the player's own install
 public:
  virtual ~StockSource() = default;
  // The decompressed bytes of stock resource (type, name); false when the install holds none. `read_from` gains the files read,
  // with their stamps.
  virtual bool ReadResource(std::uint64_t type, std::uint64_t name, std::vector<std::uint8_t>* out,
                            std::vector<StockStamp>* read_from) = 0;
  virtual bool ReadScript(std::uint64_t hash, std::vector<std::uint8_t>* out, std::vector<StockStamp>* read_from) = 0;
  virtual bool StampsCurrent(const std::vector<StockStamp>& stamps) = 0;
};

// The memory one rebuild may hold, from the device's figure: one object's share for the bytes a recipe or a script holds, the
// tables' share for a set's tables. Zero when the device gave no memory figure.
struct DeriveMemory {
  std::uint64_t object_bytes = 0;
  std::uint64_t table_bytes = 0;
};
DeriveMemory DeriveMemoryOf(const nevr_cargo::DeviceFacts& facts) noexcept;

// Rebuilds `recipe` from `stock` into *out. Insert ops index `inserts`.
std::string ApplyRecipe(const std::uint8_t* stock, std::size_t stock_size, const Recipe& recipe, const std::uint8_t* inserts,
                        std::size_t inserts_size, std::vector<std::uint8_t>* out);

// Every edit of `pin` is an 8-byte immediate of a `mov r64, imm64` (REX.W 0x48/0x49, then 0xB8-0xBF) inside an executable
// section of the PE `image`, and `to` is one of `allowed_targets`. `at` is a file offset. The caller has checked the image's
// sha256, which is what pins the value an edit replaces.
std::string CheckRekeyEdits(const std::uint8_t* image, std::size_t size, const RekeyPin& pin,
                            const std::vector<std::uint64_t>& allowed_targets);

// Writes `published_set` (empty: none) to `out_path` with the derived records merged in, appending through Env::OpenAppend and
// syncing at the end; a file already at `out_path` is cut to nothing first. The record table is in (type, name) order. The
// published chunks come first, copied byte for byte. Then each derived record is its own chunk, a zstd frame of raw blocks, in
// the order of `derived`. `produce(i, &bytes)` returns "" and the i-th derived record's verified bytes, one at a time, or a
// reason that ends the write. A derived record is aligned to 4 KiB, the largest alignment a set may ask for: the recipe does
// not carry the stock record's own alignment, and 4 KiB satisfies every smaller one. `memory` bounds the tables and the copy
// buffer it holds.
std::string WriteMergedSet(nevr_cargo::Env& env, const std::string& published_set, const std::vector<Recipe>& derived,
                           const std::function<std::string(std::size_t, std::vector<std::uint8_t>*)>& produce,
                           const std::string& out_path, const DeriveMemory& memory, std::string* sha256_out);

// The bytes of a zstd frame that holds `content_size` bytes as raw blocks (RFC 8878 section 3.1.1.2).
std::uint64_t RawFrameSize(std::uint64_t content_size);

class ChannelDeriver : public Deriver {
 public:
  ChannelDeriver(nevr_cargo::Env& env, MapFs& fs, StockSource& stock);
  // Writes <version_dir>/derived/scripts/<hash16>.dll for every rekeyed pin with edits and, when the package has derived[],
  // <version_dir>/derived/<platform>.nevrres. On a refusal, `reason` names it and the outputs are empty.
  DeriveOutput Derive(const Package& package, const std::string& version_dir,
                      const std::function<bool()>& keep_going) override;
  bool StockCurrent(const std::vector<StockStamp>& stamps) override;

 private:
  nevr_cargo::Env& env_;
  MapFs& fs_;
  StockSource& stock_;
};

}  // namespace nevr_maps
