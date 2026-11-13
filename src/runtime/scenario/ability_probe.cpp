// ability_probe.cpp: see ability_probe.h. Test builds only.
#include "runtime/scenario/ability_probe.h"

#include <array>
#include <cstdint>
#include <cstring>
#include <string>

#include "core/pch.h"
#include "abi/echovr.h"
#include "abi/echovr_functions.h"
#include "core/logging.h"
#include "nevr/pc/nevr_common.h"
#include "runtime/compat/peer_message_targets.h"
#include "runtime/hook/patching.h"
#include "runtime/hook/symbol_corpus.h"
#include "runtime/lifecycle/cli.h"     // g_isServer
#include "runtime/lifecycle/config.h"  // g_pGame
#include "runtime/lifecycle/readable_memory.h"
#include "runtime/patch/equipment_refire_rules.h"

namespace nevr_ability_probe {
namespace {

using nevr_peer_message::kPcTargets;
constexpr std::uint64_t kPeerSelf = 0xFFFFFFFFFFFFFFFCULL;
constexpr std::uint64_t kPeerInvalid = 0xFFFFFFFFFFFFFFFFULL;

using CallbackFn = void (*)(void* netGame, const void* smsg);
using ReceiveEquipmentFn = void (*)(void* component, const void* smsg);
using SendFn = void (*)(void* netGame, const void* msg, const void* buf, std::uint64_t bufLen, std::uint64_t peer,
                        std::uint64_t dest);
using AuthorityPeerFn = std::uint64_t* (*)(void* netGame, std::uint64_t* out);
using SlotOfFn = std::uint32_t* (*)(void* game, std::uint32_t* out, std::uint64_t* key);

CallbackFn g_activateOriginal = nullptr;
ReceiveEquipmentFn g_equipmentOriginal = nullptr;
SlotOfFn g_slotOf = nullptr;
void* g_component = nullptr;  // the equipment component the server's receiver last ran on (game thread)

struct Reader {
  bool operator()(std::uintptr_t address, void* out, std::size_t size) {
    if (!nevr_crash_recovery::IsReadableMemory(reinterpret_cast<const void*>(address), size)) return false;
    std::memcpy(out, reinterpret_cast<const void*>(address), size);
    return true;
  }
};

std::uintptr_t Base() { return reinterpret_cast<std::uintptr_t>(EchoVR::g_GameBaseAddress); }

std::uintptr_t NetGame() {
  Reader read;
  std::uintptr_t game = 0;
  const std::uintptr_t context = reinterpret_cast<std::uintptr_t>(g_pGame);
  if (context == 0 || !read(context + kPcTargets.contextNetGame, &game, sizeof game)) return 0;
  return game;
}

template <std::size_t N>
void* Checked(std::uint64_t va, const std::array<std::uint8_t, N>& prologue, const char* name) {
  void* fn = nevr::ResolveVA_Checked(Base(), va);
  if (fn == nullptr || !nevr::ValidatePrologue(fn, prologue.data(), prologue.size())) {
    const std::uint8_t* at = static_cast<const std::uint8_t*>(fn);
    Log(EchoVR::LogLevel::Warning, "[NEVR.SCENARIO] ability probe: %s not used va=0x%llx reason=prologue_mismatch actual=%02x%02x%02x%02x",
        name, static_cast<unsigned long long>(va), at ? at[0] : 0, at ? at[1] : 0, at ? at[2] : 0, at ? at[3] : 0);
    return nullptr;
  }
  return fn;
}

// The tac mod the server's equipment record holds for the player whose user handle this is: each record's actor key is
// asked of the game's own slot lookup (equipment_refire_rules.h layout), and the record whose answer is the handle is read
// at +0x20. Returns a word for the log: the symbol's text, "no_component" or "no_record".
std::string TacModOf(std::uintptr_t netGame, std::uint32_t handle) {
  namespace er = nevr_equipment_refire;
  if (g_component == nullptr) return "no_component";
  if (g_slotOf == nullptr) return "no_slot_lookup";
  Reader read;
  const std::uintptr_t comp = reinterpret_cast<std::uintptr_t>(g_component);
  std::uintptr_t ctx = 0, records = 0, links = 0;
  std::uint16_t count = 0;
  std::uint64_t key0 = 0;
  if (!read(comp + er::kCtxOffset, &ctx, sizeof ctx) || !read(ctx + er::kCtxActorKeyOffset, &key0, sizeof key0) ||
      !read(comp + er::kRecordsOffset, &records, sizeof records) || !read(comp + er::kLinkTableOffset, &links, sizeof links) ||
      !read(comp + er::kCountOffset, &count, sizeof count)) {
    return "component_unreadable";
  }
  for (std::uint16_t dense = 0; dense < count; ++dense) {
    std::uint64_t key[2] = {key0, 0};
    if (!read(links + static_cast<std::uintptr_t>(dense) * er::kLinkStride, &key[1], sizeof key[1])) continue;
    std::uint32_t found = 0xffffffffU;
    g_slotOf(reinterpret_cast<void*>(netGame), &found, key);
    if (found != handle) continue;
    std::uint64_t tac = 0;
    if (!read(records + static_cast<std::uintptr_t>(dense) * er::kRecordStride + er::kRecordValueOffset[1], &tac, sizeof tac)) {
      return "record_unreadable";
    }
    char text[96];
    EchoVR::FormatSymbolId(text, sizeof text, tac);
    return text;
  }
  return "no_record";
}

void ActivateHook(void* netGame, const void* smsg) {
  Reader read;
  const std::uintptr_t m = reinterpret_cast<std::uintptr_t>(smsg);
  std::uint64_t sender = 0;
  std::uintptr_t payload = 0;
  std::uint32_t msg[2] = {0, 0};
  const bool ok = read(m, &sender, sizeof sender) && read(m + kPcTargets.msgPayload, &payload, sizeof payload) &&
                  read(payload, msg, sizeof msg);
  if (ok) {
    // The callback's own resolution: slot < 16, generation equal to the net game's, then the slot's index.
    const std::uint32_t slot = msg[0] & 0xffffU;
    const std::uint32_t generation = msg[0] >> 16;
    std::uint16_t tableIndex = 0xffff, tableGeneration = 0;
    const std::uintptr_t game = reinterpret_cast<std::uintptr_t>(netGame);
    const bool resolved = slot < 16 && generation != 0xffffU &&
                          read(game + kPcTargets.slotIndexTable + 2 + slot * 4, &tableGeneration, sizeof tableGeneration) &&
                          tableGeneration == generation &&
                          read(game + kPcTargets.slotIndexTable + slot * 4, &tableIndex, sizeof tableIndex);
    const std::string tac = TacModOf(game, msg[0]);
    Log(EchoVR::LogLevel::Info,
        "[NEVR.SCENARIO] ability activate received role=%s peer=%llu handle=0x%08x slot=%u generation=%u resolved=%s index=%u "
        "active=%u tac_mod=%s (the authority's AbilityActivateCB ran for this use)",
        g_isServer ? "server" : "client", static_cast<unsigned long long>(sender), msg[0], slot, generation,
        resolved ? "yes" : "no", static_cast<unsigned>(tableIndex), msg[1], tac.c_str());
  } else {
    Log(EchoVR::LogLevel::Warning, "[NEVR.SCENARIO] ability activate received but its message could not be read smsg=%p", smsg);
  }
  g_activateOriginal(netGame, smsg);
}

void EquipmentHook(void* component, const void* smsg) {
  g_component = component;
  Reader read;
  std::uint64_t sender = 0;
  std::uintptr_t payload = 0;
  std::uint32_t user = 0;
  std::uint64_t tac = 0;
  const std::uintptr_t m = reinterpret_cast<std::uintptr_t>(smsg);
  if (read(m, &sender, sizeof sender) && read(m + kPcTargets.msgPayload, &payload, sizeof payload) &&
      read(payload, &user, sizeof user) && read(payload + 0x10, &tac, sizeof tac)) {
    char text[96];
    EchoVR::FormatSymbolId(text, sizeof text, tac);
    Log(EchoVR::LogLevel::Info, "[NEVR.SCENARIO] equipment update received role=%s peer=%llu handle=0x%08x tac_mod=%s component=%p",
        g_isServer ? "server" : "client", static_cast<unsigned long long>(sender), user, text, component);
  }
  g_equipmentOriginal(component, smsg);
}

}  // namespace

void Install() {
  if (!g_isServer) {
    Log(EchoVR::LogLevel::Info, "[NEVR.SCENARIO] ability probe: client, nothing hooked (op ability_activate sends a use)");
    return;
  }
  g_slotOf = reinterpret_cast<SlotOfFn>(Checked(kPcTargets.slotOfActor, nevr_peer_message::kPcSlotOfActorPrologue, "slot lookup"));
  int hooked = 0;
  if (void* fn = Checked(kPcTargets.abilityActivateCallback, nevr_peer_message::kPcAbilityActivateCallbackPrologue,
                         "AbilityActivateCB")) {
    g_activateOriginal = reinterpret_cast<CallbackFn>(fn);
    if (PatchDetour(&g_activateOriginal, reinterpret_cast<PVOID>(&ActivateHook), "scenario AbilityActivateCB probe")) ++hooked;
  }
  if (void* fn = Checked(kPcTargets.receiveEquipmentUpdate, nevr_peer_message::kPcReceiveEquipmentUpdatePrologue,
                         "ReceivePlayerEquipmentUpdate")) {
    g_equipmentOriginal = reinterpret_cast<ReceiveEquipmentFn>(fn);
    if (PatchDetour(&g_equipmentOriginal, reinterpret_cast<PVOID>(&EquipmentHook), "scenario equipment update probe")) ++hooked;
  }
  Log(EchoVR::LogLevel::Info, "[NEVR.SCENARIO] ability probe: server, %d of 2 read-only hooks installed", hooked);
}

std::string SendActivateForTest(bool active) {
  if (g_isServer) return "ability_activate is a client op";
  const std::uintptr_t game = NetGame();
  if (game == 0) return "no net game";
  Reader read;
  std::uint32_t handle = 0;
  if (!nevr_equipment_refire::LocalSlotHandle(read, game, &handle)) return "no local player slot";
  auto* authority = reinterpret_cast<AuthorityPeerFn>(Checked(kPcTargets.authorityPeer, nevr_peer_message::kPcAuthorityPeerPrologue,
                                                              "GetAuthorityPeer"));
  auto* send = reinterpret_cast<SendFn>(Checked(kPcTargets.abilityActivateSend, nevr_peer_message::kPcAbilityActivateSendPrologue,
                                                "Broadcast<SR15NetAbilityActivate>"));
  if (authority == nullptr || send == nullptr) return "a pinned function did not match";
  std::uintptr_t flags = 0;
  std::uint64_t flagWord = 0;
  if (!read(game + kPcTargets.netGameFlags, &flags, sizeof flags) || !read(flags, &flagWord, sizeof flagWord)) return "no role flags";
  std::uint64_t peer = kPeerInvalid;
  authority(reinterpret_cast<void*>(game), &peer);
  if (peer == kPeerInvalid || peer == kPeerSelf) return "no authority peer";
  const std::uint32_t msg[2] = {handle, active ? 1U : 0U};
  // What the cooldown node does after its log line (0x140dd8f40): Broadcast<SR15NetAbilityActivate>(game, &msg, 0, 0, peer, -1).
  send(reinterpret_cast<void*>(game), msg, nullptr, 0, peer, ~0ULL);
  Log(EchoVR::LogLevel::Info, "[NEVR.SCENARIO] ability activate sent handle=0x%08x active=%u peer=%llu flags=0x%llx (the game's own send)",
      handle, msg[1], static_cast<unsigned long long>(peer), static_cast<unsigned long long>(flagWord));
  return "";
}

}  // namespace nevr_ability_probe
