// #2242 (H-WALL-a): the UMBRAL VEIL wall's decision and renew rule. See wall_script.h for the sites that are not installed.

#include <winsock2.h>

#include "runtime/combat/wall_script.h"

#include <array>
#include <cstdio>
#include <cstring>

#include "core/logging.h"

namespace nevr_combat::wall_script {
namespace {

// mov eax, imm32; ret: the encoding of the global data length function. Built from the named length, so the two bytes the
// patch changes are the constants above and not a copied byte string.
constexpr std::array<std::uint8_t, kGlobalLenSize> MovEaxRet(std::uint32_t imm) {
  return {0xb8, static_cast<std::uint8_t>(imm & 0xff), static_cast<std::uint8_t>((imm >> 8) & 0xff),
          static_cast<std::uint8_t>((imm >> 16) & 0xff), static_cast<std::uint8_t>((imm >> 24) & 0xff), 0xc3};
}
constexpr std::array<std::uint8_t, kGlobalLenSize> kGlobalLenStockBytes = MovEaxRet(kGlobalLenStock);
constexpr std::array<std::uint8_t, kGlobalLenSize> kGlobalLenPatchedBytes = MovEaxRet(kGlobalLenWithDecision);

// The two bytes a log line names: lowercase hex, two per byte, as logging.md Rule 6 writes them.
void FormatBytes(const std::uint8_t* bytes, std::size_t n, char* out) {
  for (std::size_t i = 0; i < n; ++i) {
    static constexpr char kHex[] = "0123456789abcdef";
    out[2 * i] = kHex[bytes[i] >> 4];
    out[2 * i + 1] = kHex[bytes[i] & 0xf];
  }
  out[2 * n] = '\0';
}

// The consequence a refused patch states (Rule G): what the wall does with the stock size, or, when the writer reported an
// error after the bytes went in, what the global block now holds.
const char* ConsequenceOf(nevr_checked_patch::Result result) {
  if (result == nevr_checked_patch::Result::WrittenWithError) {
    return "the length bytes read back as patched though the writer reported an error; the wall's global block is the patched size";
  }
  return "the wall runs with its stock global data size; no veil decision cell is written";
}

}  // namespace

bool TakeVeilToken(std::uint8_t* token, std::uint64_t now) {
  auto* count = reinterpret_cast<std::int32_t*>(token + kTokenCount);
  const auto* expiry = reinterpret_cast<const std::uint64_t*>(token + kTokenExpiry);
  std::int32_t seen = __atomic_load_n(count, __ATOMIC_SEQ_CST);
  for (;;) {
    if (seen <= 0 || now >= __atomic_load_n(expiry, __ATOMIC_SEQ_CST)) return false;
    if (__atomic_compare_exchange_n(count, &seen, seen - 1, /*weak=*/false, __ATOMIC_SEQ_CST, __ATOMIC_SEQ_CST)) break;
  }
  __atomic_fetch_add(reinterpret_cast<std::int32_t*>(token + kTokenShown), 1, __ATOMIC_SEQ_CST);
  return true;
}

Decision CellDecision(std::uint32_t cell) {
  const std::uint32_t byte = cell & kDecisionMask;
  if (byte == static_cast<std::uint32_t>(Decision::kVeil)) return Decision::kVeil;
  if (byte == static_cast<std::uint32_t>(Decision::kBarrier)) return Decision::kBarrier;
  return Decision::kUndecided;
}

std::uint32_t WithDecision(std::uint32_t cell, Decision decision) {
  return (cell & ~kDecisionMask) | static_cast<std::uint32_t>(decision);
}

std::uint32_t CellClock(std::uint32_t cell) { return (cell >> kCellClockShift) & kClockMask; }

Verdict Decide(std::uint8_t* token, std::uint32_t& cell, std::uint64_t now, bool decideForGood) {
  const Decision kept = CellDecision(cell);
  if (kept == Decision::kVeil) return Verdict::kVeil;
  if (kept == Decision::kBarrier) return Verdict::kBarrier;
  if (TakeVeilToken(token, now)) {
    cell = WithDecision(cell, Decision::kVeil);
    return Verdict::kVeil;
  }
  if (!decideForGood) return Verdict::kUndecided;
  cell = WithDecision(cell, Decision::kBarrier);
  __atomic_fetch_add(reinterpret_cast<std::int32_t*>(token + kTokenBarriers), 1, __ATOMIC_SEQ_CST);
  return Verdict::kBarrier;
}

bool NewLife(std::uint32_t& cell, std::uint64_t now, std::uint32_t afterUnits) {
  const std::uint32_t units = static_cast<std::uint32_t>(now >> kTimeShift) & kClockMask;
  const std::uint32_t elapsed = (units - CellClock(cell)) & kClockMask;
  cell = (units << kCellClockShift) | (cell & kDecisionMask);
  return elapsed >= afterUnits;
}

bool RenewOnEntry(std::uint32_t& cell, bool aborting, std::uint64_t now, std::uint32_t afterUnits) {
  if (aborting) return false;
  if (!NewLife(cell, now, afterUnits)) return false;
  cell = WithDecision(cell, Decision::kUndecided);
  return true;
}

nevr_checked_patch::Site GlobalLenSite() {
  return nevr_checked_patch::Site{kGlobalLenStockBytes.data(), kGlobalLenPatchedBytes.data(), kGlobalLenSize};
}

GlobalLenOutcome ApplyGlobalLen(std::uint8_t* fn, nevr_checked_patch::WriteFn write, nevr_checked_patch::WriteErrorFn writeError) {
  GlobalLenOutcome outcome{};
  // The bytes found before any write: Apply writes over them on a success, and a refused write's record names them as found.
  std::memcpy(outcome.found, fn, kGlobalLenSize);
  outcome.result = nevr_checked_patch::Apply(fn, GlobalLenSite(), write);
  outcome.writeError = writeError();
  return outcome;
}

int FormatGlobalLenRecord(char* out, std::size_t cap, std::uintptr_t va, const GlobalLenOutcome& outcome) {
  char expected[2 * kGlobalLenSize + 1];
  char actual[2 * kGlobalLenSize + 1];
  FormatBytes(kGlobalLenStockBytes.data(), kGlobalLenSize, expected);
  FormatBytes(outcome.found, kGlobalLenSize, actual);
  int n = std::snprintf(out, cap,
                        "[NEVR.COMBAT] wall script patch not applied name=wall_global_len va=0x%llx expected=%s actual=%s result=%s",
                        static_cast<unsigned long long>(va), expected, actual, nevr_checked_patch::ResultName(outcome.result));
  const bool writeFailed = outcome.result == nevr_checked_patch::Result::WriteFailed ||
                           outcome.result == nevr_checked_patch::Result::WrittenWithError;
  if (writeFailed && n >= 0 && static_cast<std::size_t>(n) < cap) {
    n += std::snprintf(out + n, cap - static_cast<std::size_t>(n), " write_error=%u", outcome.writeError);
  }
  if (n >= 0 && static_cast<std::size_t>(n) < cap) {
    n += std::snprintf(out + n, cap - static_cast<std::size_t>(n), " consequence=%s", ConsequenceOf(outcome.result));
  }
  return n;
}

nevr_checked_patch::Result PatchGlobalLen(std::uint8_t* fn, std::uintptr_t va, nevr_checked_patch::WriteFn write,
                                          nevr_checked_patch::WriteErrorFn writeError) {
  const GlobalLenOutcome outcome = ApplyGlobalLen(fn, write, writeError);
  if (outcome.result == nevr_checked_patch::Result::Applied || outcome.result == nevr_checked_patch::Result::AlreadyApplied) {
    return outcome.result;
  }
  char record[512];
  FormatGlobalLenRecord(record, sizeof record, va, outcome);
  Log(EchoVR::LogLevel::Warning, "%s", record);
  return outcome.result;
}

}  // namespace nevr_combat::wall_script
