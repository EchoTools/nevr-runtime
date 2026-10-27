// The Quest side of #970: the project section of the CREDITS page (see credits_text.h for the sites and the
// encodings). Install runs once, at the boot step after libr15 is loaded, before the game opens the page.
//
// Order, each step refusing the whole patch on failure so the game never runs with one pair retargeted and
// the other not (the page would then start on one text and reset to the other):
//   1. libr15 is loaded, its build ID is the pinned one;
//   2. the block the build rendered passes ValidateBlock (the runtime's second guard, as on PC);
//   3. the four pinned words are at the two pairs (nothing else is written on a mismatch);
//   4. the buffer (block, then libr15's own blob bytes up to NUL, then NUL) is built in a page within ADRP
//      reach of both pairs;
//   5. the four words are written, each read back, and all of them are put back if one write fails.
//
// Log: one `credits_text` line per outcome (sentinel log, LogFields). The line names the game bytes' SHA-256,
// so a headset run can be compared with the PC blob (src/runtime/patch/credits_text.h kBlobSha256).

#include "credits_text.h"

#include <sys/mman.h>
#include <unistd.h>

#include <cstdint>
#include <cstdio>
#include <cstring>
#include <string>
#include <vector>

#include "quest/data/sha256.h"
#include "got_hook.h"
#include "hook_log.h"
#include "pinned_targets.h"
#include "runtime/patch/credits_block.h"
#include "runtime/patch/credits_text.h"

namespace nevr_quest::credits_text {
namespace {

// The blob is read up to its NUL and no further. The PC blob is 30091 bytes; this bound only keeps a missing NUL
// from reading to the end of a mapping.
constexpr std::size_t kMaxBlobBytes = 64 * 1024;
// ADRP reaches +-4 GiB from the instruction; the buffer must be placed inside that.
constexpr std::uint64_t kReachBytes = std::uint64_t{1} << 32;
constexpr std::uint64_t kHintStepBytes = std::uint64_t{16} << 20;

const char* SiteName(const PairSite& site) {
  return &site == &kInitSite ? "init" : "disabled";
}

void Refuse(sentinel::LogLevel level, const char* reason) {
  sentinel::LogFields(level, "credits_text", {{"status", "skipped"}, {"reason", reason}});
}

// A page within ADRP reach of both pairs, or nullptr. Asks the kernel for addresses near the image and keeps the
// first one the encoder accepts; the kernel may place the mapping elsewhere, in which case it is released.
void* AllocateInReach(std::uintptr_t base, std::size_t size) {
  for (std::uint64_t step = kHintStepBytes; step < kReachBytes; step += kHintStepBytes) {
    for (int dir = 0; dir < 2; ++dir) {
      if (dir == 1 && step > base) continue;
      const std::uintptr_t hint = dir == 0 ? base + step : base - step;
      void* const p = mmap(reinterpret_cast<void*>(hint), size, PROT_READ | PROT_WRITE, MAP_PRIVATE | MAP_ANONYMOUS, -1, 0);
      if (p == MAP_FAILED) continue;
      const auto addr = reinterpret_cast<std::uintptr_t>(p);
      PairWords init{};
      PairWords disabled{};
      if (PlanPair(kInitSite, base, addr, &init) && PlanPair(kDisabledSite, base, addr, &disabled)) return p;
      munmap(p, size);
    }
  }
  return nullptr;
}

struct Word {
  std::uintptr_t addr;
  std::uint32_t before;
  std::uint32_t after;
};

// Writes one 4-byte instruction in a page the patch makes writable for the write and executable again after it.
bool WriteWord(std::uintptr_t addr, std::uint32_t word) {
  const auto page = static_cast<std::uintptr_t>(sysconf(_SC_PAGESIZE));
  void* const pageStart = reinterpret_cast<void*>(addr & ~(page - 1));
  if (mprotect(pageStart, page, PROT_READ | PROT_WRITE) != 0) return false;
  std::memcpy(reinterpret_cast<void*>(addr), &word, sizeof(word));
  const bool restored = mprotect(pageStart, page, PROT_READ | PROT_EXEC) == 0;
  __builtin___clear_cache(reinterpret_cast<char*>(addr), reinterpret_cast<char*>(addr + sizeof(word)));
  return restored;
}

std::uint32_t ReadWord(std::uintptr_t addr) {
  std::uint32_t word = 0;
  std::memcpy(&word, reinterpret_cast<const void*>(addr), sizeof(word));
  return word;
}

std::string Hex(std::uint64_t value) {
  char text[32];
  std::snprintf(text, sizeof(text), "0x%llx", static_cast<unsigned long long>(value));
  return text;
}

}  // namespace

bool InstallCreditsText() {
  sentinel::ElfImage image;
  if (!sentinel::FindLoadedImage(sentinel::pinned::kLibR15, &image)) {
    Refuse(sentinel::LogLevel::kWarn, "module_not_loaded");
    return false;
  }
  char id[64] = {};
  if (!sentinel::ReadBuildId(image, id, sizeof(id))) {
    Refuse(sentinel::LogLevel::kError, "no_build_id");
    return false;
  }
  if (std::strcmp(id, sentinel::pinned::kLibR15BuildId) != 0) {
    Refuse(sentinel::LogLevel::kError, "build_id_mismatch");
    return false;
  }

  const char* const block = nevr_credits_block::kText;
  nevr_credits_text::BlockStats stats;
  const nevr_credits_text::BlockStatus status = nevr_credits_text::ValidateBlock(block, nevr_credits_block::kSize, &stats);
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

  for (const PairSite* site : {&kInitSite, &kDisabledSite}) {
    const std::uintptr_t adrpAddr = image.base + site->adrpVaddr;
    const std::uintptr_t addAddr = image.base + site->addVaddr;
    const std::uint32_t adrp = ReadWord(adrpAddr);
    const std::uint32_t add = ReadWord(addAddr);
    if (!PairHoldsPinned(*site, adrp, add)) {
      sentinel::LogFields(sentinel::LogLevel::kError, "credits_text",
                          {{"status", "refused"},
                           {"reason", "prologue_mismatch"},
                           {"site", SiteName(*site)},
                           {"adrp", Hex(adrp).c_str()},
                           {"add", Hex(add).c_str()}});
      return false;
    }
  }

  const std::vector<std::uint8_t> buffer = nevr_credits_text::BuildBuffer(block, nevr_credits_block::kSize, blob, gameLen);
  void* const memory = AllocateInReach(image.base, buffer.size());
  if (memory == nullptr) {
    Refuse(sentinel::LogLevel::kError, "no_buffer_in_range");
    return false;
  }
  std::memcpy(memory, buffer.data(), buffer.size());
  if (mprotect(memory, buffer.size(), PROT_READ) != 0) {
    munmap(memory, buffer.size());
    Refuse(sentinel::LogLevel::kError, "buffer_protect_failed");
    return false;
  }

  const auto bufferVa = reinterpret_cast<std::uintptr_t>(memory);
  std::vector<Word> words;
  for (const PairSite* site : {&kInitSite, &kDisabledSite}) {
    PairWords plan{};
    if (!PlanPair(*site, image.base, bufferVa, &plan)) {
      munmap(memory, buffer.size());
      Refuse(sentinel::LogLevel::kError, "out_of_range");
      return false;
    }
    words.push_back({image.base + site->adrpVaddr, site->adrpExpected, plan.adr});
    words.push_back({image.base + site->addVaddr, site->addExpected, plan.add});
  }

  std::size_t written = 0;
  bool ok = true;
  for (; written < words.size(); ++written) {
    if (!WriteWord(words[written].addr, words[written].after)) {
      ok = false;
      break;
    }
  }
  if (ok) {
    for (const Word& w : words) {
      if (ReadWord(w.addr) != w.after) {
        ok = false;
        break;
      }
    }
  }
  if (!ok) {
    // Put back every word that was written, the failing one included (a partly written word cannot be told
    // apart from one that was not, so both are restored to the pinned value).
    for (std::size_t i = 0; i <= written && i < words.size(); ++i) WriteWord(words[i].addr, words[i].before);
    munmap(memory, buffer.size());
    Refuse(sentinel::LogLevel::kError, "write_failed");
    return false;
  }

  sentinel::LogFields(sentinel::LogLevel::kInfo, "credits_text",
                      {{"status", "applied"},
                       {"sites", "init,disabled"},
                       {"block_lines", stats.lines},
                       {"block_bytes", nevr_credits_block::kSize},
                       {"game_bytes", gameLen},
                       {"total_bytes", buffer.size()},
                       {"game_sha256", gameSha.c_str()},
                       {"buffer", Hex(bufferVa).c_str()}});
  return true;
}

}  // namespace nevr_quest::credits_text
