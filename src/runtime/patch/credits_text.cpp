#include "runtime/patch/credits_text.h"

#include <cstdint>
#include <cstring>
#include <string>

#include "abi/echovr_functions.h"
#include "core/logging.h"
#include "runtime/hook/near_alloc.h"
#include "runtime/hook/process_memory.h"
#include "runtime/patch/asset_cache.h"
#include "runtime/patch/credits_text_install.h"

namespace nevr_credits_text {

namespace {

bool ProcessWrite(void* dest, const void* src, size_t n) { return ProcessMemcpy(dest, src, n); }

void Skip(const char* reason, bool warn) {
  Log(warn ? EchoVR::LogLevel::Warning : EchoVR::LogLevel::Info,
      "[NEVR.PATCH] credits_text skipped reason=%s (the game's own credits are unchanged)", reason);
}

}  // namespace

bool Install(const char* block, size_t blockLen) {
  BlockStats stats;
  const BlockStatus status = ValidateBlock(block, blockLen, &stats);
  if (status == BlockStatus::Empty) {
    Skip(BlockStatusName(status), false);
    return false;
  }
  if (status != BlockStatus::Ok) {
    Skip(BlockStatusName(status), true);
    return false;
  }

  uint8_t* const base = reinterpret_cast<uint8_t*>(EchoVR::g_GameBaseAddress);
  if (base == nullptr) {
    Skip("no_game_base", true);
    return false;
  }

  // The game's own bytes: its credits blob, taken from the mapped image, not from any file.
  const uint8_t* const blob = base + kBlobRva;
  const void* const nul = std::memchr(blob, 0, kBlobLength + 1);
  const size_t found = nul != nullptr ? static_cast<size_t>(static_cast<const uint8_t*>(nul) - blob) : kBlobLength + 1;
  const std::string sha = found == kBlobLength ? nevr_asset_cache::Sha256Hex(blob, found) : std::string();
  const BlobIdentity identity = IdentifyBlob(found, sha.c_str());
  if (identity != BlobIdentity::Match) {
    Skip(BlobIdentityName(identity), true);
    return false;
  }

  SitePlan init{base + kInitSiteRva, reinterpret_cast<uint64_t>(base + kInitSiteRva), kInitSiteExpected, {}};
  SitePlan disabled{base + kDisabledSiteRva, reinterpret_cast<uint64_t>(base + kDisabledSiteRva), kDisabledSiteExpected,
                    {}};
  uint8_t initBefore[kSiteSize];
  uint8_t disabledBefore[kSiteSize];
  std::memcpy(initBefore, init.site, kSiteSize);
  std::memcpy(disabledBefore, disabled.site, kSiteSize);

  const std::vector<uint8_t> buffer = BuildBuffer(block, blockLen, blob, found);
  void* const memory = nevr_near_alloc::AllocateNear(static_cast<uintptr_t>(init.siteVa), buffer.size());
  if (memory == nullptr) {
    Skip("no_buffer_in_range", true);
    return false;
  }
  std::memcpy(memory, buffer.data(), buffer.size());
  DWORD oldProtect = 0;
  if (VirtualProtect(memory, buffer.size(), PAGE_READONLY, &oldProtect) == 0) {
    // Still correct to use, but the page text should not be writable; refuse rather than ship it so.
    VirtualFree(memory, 0, MEM_RELEASE);
    Skip("buffer_protect_failed", true);
    return false;
  }

  const Outcome outcome = Retarget(init, disabled, reinterpret_cast<uint64_t>(memory), ProcessWrite);

  char record[256];
  FormatSiteRecord(record, sizeof(record), "init", init.siteVa, kInitSiteExpected, initBefore, outcome);
  const bool ok = outcome == Outcome::Applied || outcome == Outcome::AlreadyApplied;
  Log(ok ? EchoVR::LogLevel::Info : EchoVR::LogLevel::Warning, "%s", record);
  FormatSiteRecord(record, sizeof(record), "disabled", disabled.siteVa, kDisabledSiteExpected, disabledBefore, outcome);
  Log(ok ? EchoVR::LogLevel::Info : EchoVR::LogLevel::Warning, "%s", record);

  if (!ok) {
    VirtualFree(memory, 0, MEM_RELEASE);
    Skip(OutcomeName(outcome), true);
    return false;
  }
  FormatBuiltRecord(record, sizeof(record), stats.lines, blockLen, stats.longestLine, stats.overReview, found,
                    buffer.size(), reinterpret_cast<uint64_t>(memory));
  Log(EchoVR::LogLevel::Info, "%s", record);
  return true;
}

}  // namespace nevr_credits_text
