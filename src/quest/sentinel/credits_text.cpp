// The Quest side of #970: the project section of the CREDITS page (see credits_text.h). Install runs once, at the boot
// step after libr15 is loaded and Attach has verified it, before the game opens the page.
//
// Order, each step refusing the whole install on failure (the slot then keeps the game's own text):
//   1. libr15 is loaded and Attach verified it (the credits slot exists): otherwise `libr15_not_patched` when the live
//      build ID is the stock one, `not_verified` for anything else;
//   2. the block the build rendered passes ValidateBlock (the runtime's second guard, as on PC);
//   3. the buffer (block, then libr15's own blob bytes up to NUL, then NUL) is built in anonymous memory and made read-only;
//   4. the buffer's address is stored in the credits slot with one atomic store. The two patched ADRP+LDR pairs read it
//      from there. No code page is written and no protection of the image changes.
//
// Log: one `credits_text` line per outcome (sentinel log, LogFields). The line names the game bytes' SHA-256, so a headset
// run can be compared with the PC blob (src/runtime/patch/credits_text.h kBlobSha256).

#include "credits_text.h"

#include <sys/mman.h>

#include <cstdint>
#include <cstring>
#include <string>
#include <vector>

#include "hook_log.h"
#include "pinned_targets.h"
#include "quest/data/sha256.h"
#include "quest/patch/libr15_patch_table.h"
#include "runtime/patch/credits_block.h"
#include "runtime/patch/credits_text.h"

namespace nevr_quest::credits_text {
namespace {

// The blob is read up to its NUL and no further. The PC blob is 30091 bytes; this bound only keeps a missing NUL
// from reading to the end of a mapping.
constexpr std::size_t kMaxBlobBytes = 64 * 1024;

void Refuse(sentinel::LogLevel level, const char* reason) {
  sentinel::LogFields(level, "credits_text", {{"status", "skipped"}, {"reason", reason}});
}

}  // namespace

bool InstallCreditsText(const InstallOptions& options) {
  sentinel::ElfImage image;
  if (options.lookup == nullptr || !options.lookup(sentinel::pinned::kLibR15, &image)) {
    Refuse(sentinel::LogLevel::kWarn, "module_not_loaded");
    return false;
  }
  char id[64] = {};
  if (!sentinel::ReadBuildId(image, id, sizeof(id))) {
    Refuse(sentinel::LogLevel::kError, "no_build_id");
    return false;
  }
  void** const slot = libr15_patch::CreditsSlot();
  if (slot == nullptr) {
    Refuse(sentinel::LogLevel::kError,
           std::strcmp(id, patch::kLibR15StockBuildId) == 0 ? libr15_patch::kReasonNotPatched : "not_verified");
    return false;
  }

  const char* const block = options.block != nullptr ? options.block : nevr_credits_block::kText;
  const std::size_t blockSize = options.block != nullptr ? options.blockSize : nevr_credits_block::kSize;
  nevr_credits_text::BlockStats stats;
  const nevr_credits_text::BlockStatus status = nevr_credits_text::ValidateBlock(block, blockSize, &stats);
  if (status != nevr_credits_text::BlockStatus::Ok) {
    Refuse(sentinel::LogLevel::kWarn, nevr_credits_text::BlockStatusName(status));
    return false;
  }

  const auto* const blob = reinterpret_cast<const std::uint8_t*>(image.base + kBlobVaddr);
  const void* const nul = std::memchr(blob, 0, kMaxBlobBytes);
  if (nul == nullptr) {
    Refuse(sentinel::LogLevel::kError, "blob_not_terminated");
    return false;
  }
  const std::size_t gameLen = static_cast<std::size_t>(static_cast<const std::uint8_t*>(nul) - blob);
  const std::string gameSha = nevr_quest::data::Sha256Hex(blob, gameLen);

  const std::vector<std::uint8_t> buffer = nevr_credits_text::BuildBuffer(block, blockSize, blob, gameLen);
  const libr15_patch::MapFn map = options.map != nullptr ? options.map : &mmap;
  const libr15_patch::ProtectFn protect = options.protect != nullptr ? options.protect : &mprotect;
  void* const memory = map(nullptr, buffer.size(), PROT_READ | PROT_WRITE, MAP_PRIVATE | MAP_ANONYMOUS, -1, 0);
  if (memory == MAP_FAILED) {
    Refuse(sentinel::LogLevel::kError, "buffer_map_failed");
    return false;
  }
  std::memcpy(memory, buffer.data(), buffer.size());
  // The one protection change this file makes, and on its own anonymous buffer: never on a page of the image.
  if (protect(memory, buffer.size(), PROT_READ) != 0) {
    munmap(memory, buffer.size());
    Refuse(sentinel::LogLevel::kError, "buffer_protect_failed");
    return false;
  }

  __atomic_store_n(slot, memory, __ATOMIC_RELEASE);

  char bufferText[19];
  char slotText[19];
  sentinel::LogFields(sentinel::LogLevel::kInfo, "credits_text",
                      {{"status", "applied"},
                       {"sites", "init,disabled"},
                       {"block_lines", stats.lines},
                       {"block_bytes", blockSize},
                       {"game_bytes", gameLen},
                       {"total_bytes", buffer.size()},
                       {"game_sha256", gameSha.c_str()},
                       {"buffer", sentinel::HexString(bufferText, reinterpret_cast<std::uintptr_t>(memory))},
                       {"slot", sentinel::HexString(slotText, reinterpret_cast<std::uintptr_t>(slot))}});
  return true;
}

bool InstallCreditsText() { return InstallCreditsText(InstallOptions{}); }

}  // namespace nevr_quest::credits_text
