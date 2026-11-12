#include "runtime/patch/new_unlocks_clear.h"

#include <windows.h>

#include <array>
#include <cstddef>
#include <cstdint>
#include <cstdio>
#include <cstring>

#include "abi/echovr.h"
#include "core/logging.h"
#include "nevr/pc/nevr_common.h"
#include "runtime/compat/social_party.h"
#include "runtime/hook/hook_guard.h"
#include "runtime/hook/patching.h"
#include "runtime/lifecycle/readable_memory.h"
#include "runtime/log/boot_log_tee.h"
#include "runtime/patch/new_unlocks_clear_rules.h"

namespace nevr_new_unlocks_clear {
namespace {

using nevr_crash_recovery::IsReadableMemory;

// The first 16 bytes of each function, from the disassembly of echovr.exe (the same bytes the scenario control pins for the clear).
constexpr std::array<std::uint8_t, 16> kClearPrologue = {0x40, 0x53, 0x48, 0x83, 0xEC, 0x20, 0x48, 0x83,
                                                         0xB9, 0x50, 0x1C, 0x05, 0x00, 0x00, 0x48, 0x8B};
constexpr std::array<std::uint8_t, 16> kJsonEmptyPrologue = {0x48, 0x83, 0xEC, 0x28, 0x48, 0x8D, 0x15, 0x74,
                                                             0xB9, 0x0D, 0x01, 0xE8, 0xF0, 0xE6, 0xFF, 0xFF};

using ClearFn = void (*)(void* netGame);
using JsonEmptyFn = int (*)(void* json);

ClearFn g_clearOriginal = nullptr;
JsonEmptyFn g_jsonEmpty = nullptr;
bool g_installed = false;
Hold g_hold;                   // the clear hook and OnFrame run on the game loop's thread (new_unlocks_clear_rules.h, "Threads")
void* g_heldNetGame = nullptr;  // the net game the held clear was made on: its user is re-read before the dirty bit is touched

std::uint8_t* Bytes(void* p) { return static_cast<std::uint8_t*>(p); }

// The game's own test for "an update was sent and is not yet acknowledged": the buffer at user+0x28 is not empty.
bool UpdateInFlight(void* user) { return g_jsonEmpty(Bytes(user) + kUserInFlightOffset) == 0; }

// The account the line is about: the local user's id, the one the lobby records carry (0 until the social layer knows it).
unsigned long long LocalUserId() { return static_cast<unsigned long long>(nevr_social_party::Global().Snapshot().selfId); }

bool ReadU64(const void* address, std::uint64_t* out) {
  if (!IsReadableMemory(address, sizeof(*out))) return false;
  std::memcpy(out, address, sizeof(*out));
  return true;
}

void ClearHook(void* netGame) {
  std::uint64_t before = 0;
  void* user = nullptr;
  const bool readable = ReadU64(Bytes(netGame) + kNewUnlocksCountOffset, &before) &&
                        ReadU64(Bytes(netGame) + kNetGameUserOffset, reinterpret_cast<std::uint64_t*>(&user)) &&
                        user != nullptr && IsReadableMemory(user, kUserFlagsOffset + sizeof(std::uint32_t));
  g_clearOriginal(netGame);
  if (!readable) return;  // the game's clear ran as it always did; there is nothing the rules can read
  if (g_hold.OnClear(user, before, UpdateInFlight(user)) == ClearVerdict::kHeld) {
    g_heldNetGame = netGame;
    Log(EchoVR::LogLevel::Info, "[NEVR.UNLOCKS] %s user_id=%llu count=%llu", kMsgHeld, LocalUserId(),
        static_cast<unsigned long long>(g_hold.HeldCount()));
  }
}

void HexBytes(const std::uint8_t* bytes, std::size_t size, char* out, std::size_t outSize) {
  std::size_t at = 0;
  out[0] = '\0';
  for (std::size_t i = 0; i < size && at + 3 <= outSize; ++i) at += static_cast<std::size_t>(std::snprintf(out + at, outSize - at, "%02x", bytes[i]));
}

// True when the bytes at va are `expected`. A mismatch is a Warning with the address, the name, the first differing offset and the bytes of
// each side (logging.md Rule 6), and the caller installs nothing.
bool Matches(std::uintptr_t gameBase, std::uint64_t va, const std::array<std::uint8_t, 16>& expected, const char* name) {
  void* target = nevr::ResolveVA_Checked(gameBase, va);
  if (target != nullptr && nevr::ValidatePrologue(target, expected.data(), expected.size())) return true;
  std::size_t differs = 0;
  char want[40] = "";
  char have[40] = "unreadable";
  HexBytes(expected.data(), expected.size(), want, sizeof want);
  if (target != nullptr && IsReadableMemory(target, expected.size())) {
    const auto* actual = static_cast<const std::uint8_t*>(target);
    while (differs < expected.size() && actual[differs] == expected[differs]) ++differs;
    HexBytes(actual, expected.size(), have, sizeof have);
  }
  nevr_boot_log_tee::TeeFprintf(
      "[NEVR.UNLOCKS] warn; new unlocks clear hook not installed: %s va=0x%llx reason=bytes_mismatch first_difference=%zu expected=%s actual=%s "
      "(the game build is not the one this was written against, so a clear made while an update is unacknowledged is left as the game makes it)\n",
      name, static_cast<unsigned long long>(va), differs, want, have);
  return false;
}

}  // namespace

void Install(std::uintptr_t gameBase) {
  const Addresses& a = kPcAddresses;
  bool ok = Matches(gameBase, a.clear, kClearPrologue, "ClearNewUnlocks");
  ok = Matches(gameBase, a.jsonEmpty, kJsonEmptyPrologue, "CJsonEmpty") && ok;
  if (!ok) {
    nevr_boot_log_tee::TeeFprintf("[NEVR.UNLOCKS] warn; new unlocks clear hook: 0 of 1 installed (a pinned check failed, see above)\n");
    return;
  }
  g_clearOriginal = reinterpret_cast<ClearFn>(nevr::ResolveVA_Checked(gameBase, a.clear));
  g_jsonEmpty = reinterpret_cast<JsonEmptyFn>(nevr::ResolveVA_Checked(gameBase, a.jsonEmpty));
  g_installed = PatchDetour(&g_clearOriginal, reinterpret_cast<void*>(&ClearHook), "ClearNewUnlocks") != FALSE;
  nevr_boot_log_tee::TeeFprintf(
      "[NEVR.UNLOCKS] %s; new unlocks clear hook: %d of 1 installed (a clear made while a profile update is unacknowledged is sent after the "
      "acknowledgement)\n",
      g_installed ? "info" : "warn", g_installed ? 1 : 0);
}

void OnFrame() {
  void* const held = const_cast<void*>(g_hold.HeldUser());
  if (!g_installed || held == nullptr) return;  // one pointer test per frame while nothing is held
  const unsigned long long heldCount = static_cast<unsigned long long>(g_hold.HeldCount());
  // The user the clear was held for must still be the net game's user: after a log-out the pointer can be freed or reused, and the dirty bit
  // of another object is never touched.
  std::uint64_t current = 0;
  std::uint64_t countNow = 0;
  if (!ReadU64(Bytes(g_heldNetGame) + kNetGameUserOffset, &current) || reinterpret_cast<void*>(current) != held ||
      !IsReadableMemory(held, kUserFlagsOffset + sizeof(std::uint32_t)) ||
      !ReadU64(Bytes(g_heldNetGame) + kNewUnlocksCountOffset, &countNow)) {
    g_hold.Drop();
    g_heldNetGame = nullptr;
    Log(EchoVR::LogLevel::Info, "[NEVR.UNLOCKS] %s user_id=%llu count=%llu", kMsgDropped, LocalUserId(), heldCount);
    return;
  }
  const PollVerdict verdict = g_hold.OnPoll(held, UpdateInFlight(held), countNow);
  if (verdict == PollVerdict::kSuperseded) {
    g_heldNetGame = nullptr;
    Log(EchoVR::LogLevel::Info, "[NEVR.UNLOCKS] %s user_id=%llu count=%llu", kMsgSuperseded, LocalUserId(), heldCount);
  }
  if (verdict == PollVerdict::kRearm) {
    // An atomic OR: the game's own code sets and clears the same word (the acknowledgement handler ORs this bit too).
    InterlockedOr(reinterpret_cast<volatile LONG*>(Bytes(held) + kUserFlagsOffset), static_cast<LONG>(kUserDirtyBit));
    g_heldNetGame = nullptr;
    Log(EchoVR::LogLevel::Info, "[NEVR.UNLOCKS] %s user_id=%llu count=%llu", kMsgRearmed, LocalUserId(),
        static_cast<unsigned long long>(g_hold.LastCount()));
  }
}

}  // namespace nevr_new_unlocks_clear
