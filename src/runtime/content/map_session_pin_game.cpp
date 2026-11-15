// The session pin's PC half: echovr.exe's owner-JSON setter and getter (addresses.h), the lobby and the net game, the error block
// and the return to lobby. Each function's entry is checked before it is called; a build that differs leaves the pin off and says
// so once, with the function's name, its VA and the expected and actual bytes (logging.md Rule 6). Every line the pin writes is
// formatted here (the core only decides and emits PinEvent records).

#include <winsock2.h>  // before windows.h (tools/tests/test_header_include_order.py)
#include <windows.h>

#include <cstddef>
#include <cstdint>
#include <cstring>
#include <string>

#include "abi/game_level.h"
#include "core/logging.h"
#include "core/map_session_pin.h"
#include "runtime/content/map_session_pin_entry.h"
#include "runtime/content/map_session_pin_game.h"
#include "runtime/content/map_session_pin_notice.h"
#include "runtime/hook/addresses.h"
#include "runtime/lifecycle/cli.h"
#include "runtime/lifecycle/config.h"  // g_pGame (the game context)
#include "runtime/lifecycle/return_to_lobby.h"
#include "runtime/lifecycle/return_to_lobby_queue.h"
#include "runtime/patch/lobby_failure_message_rules.h"

namespace nevr_map_session_pin {

namespace {

using OwnerSetSymbolFn = void(__fastcall*)(std::uint64_t lobby, const char* key, std::uint64_t value);
using OwnerGetSymbolFn = std::uint64_t*(__fastcall*)(std::uint64_t lobby, std::uint64_t* out, const char* key,
                                                      std::uint64_t fallback);

// The owner-JSON getter's fallback: the pin reads 0 when the key is absent (the core's "0 when absent").
constexpr std::uint64_t kOwnerSymbolAbsent = 0;
// The names the records give the two functions (the setter's is unconfirmed: revault fcn.140615e60).
constexpr const char* kOwnerSetterName = "CNSLobby owner setter (fcn.140615e60)";
constexpr const char* kOwnerGetterName = "CNSLobby::OwnerJson_GetSymbol";

std::uint8_t* GameBase() { return reinterpret_cast<std::uint8_t*>(GetModuleHandleW(nullptr)); }

// One `map_pin status=off` record, the fields in the order the core gave them.
void LogPinOff(const PinFields& fields) {
  Log(EchoVR::LogLevel::Warning, "[NEVR.MAPS] map_pin %s", FormatFields(fields).c_str());
}

// Both owner-JSON functions have their stock entry. The first that does not logs its record and fails the check.
bool ProveEntries(const std::uint8_t* base) {
  PinFields refusal;
  if (nevr_map_session_pin_entry::CheckEntry(base, nevr_patch_addresses::CNSLOBBY_OWNER_SYMBOL_SET,
                                             nevr_patch_addresses::CNSLOBBY_OWNER_SYMBOL_SET_PROLOGUE,
                                             sizeof(nevr_patch_addresses::CNSLOBBY_OWNER_SYMBOL_SET_PROLOGUE),
                                             "owner_setter_mismatch", kOwnerSetterName, refusal) &&
      nevr_map_session_pin_entry::CheckEntry(base, nevr_patch_addresses::CNSLOBBY_OWNER_SYMBOL_GET,
                                             nevr_patch_addresses::CNSLOBBY_OWNER_SYMBOL_GET_PROLOGUE,
                                             sizeof(nevr_patch_addresses::CNSLOBBY_OWNER_SYMBOL_GET_PROLOGUE),
                                             "owner_getter_mismatch", kOwnerGetterName, refusal)) {
    return true;
  }
  LogPinOff(refusal);
  return false;
}

// The log line for one record the core emitted. The shape is the one docs/standards/logging.md [NEVR.MAPS] states, the same on
// the Quest: `map_skew reason=<...>` for every client outcome. A notice is quoted; only ids, levels, pins and tokens are written.
void WriteEvent(const PinEvent& e) {
  const std::string level = HexText(e.level);
  const std::string pin = HexText(e.pin);
  switch (e.kind) {
    case PinEventKind::kServerSet:
      Log(EchoVR::LogLevel::Info, "[NEVR.MAPS] session pin set level=%s pin=%s", level.c_str(), pin.c_str());
      return;
    case PinEventKind::kServerCleared:
      Log(EchoVR::LogLevel::Info, "[NEVR.MAPS] session pin cleared level=%s", level.c_str());
      return;
    case PinEventKind::kUnpinned:
      Log(EchoVR::LogLevel::Info, "[NEVR.MAPS] map_skew reason=map_unpinned id=%s level=%s", e.id.c_str(), level.c_str());
      return;
    case PinEventKind::kLocalMap:
      Log(EchoVR::LogLevel::Warning, "[NEVR.MAPS] map_skew reason=local_map id=%s level=%s pin=%s local=\"%s\"", e.id.c_str(),
          level.c_str(), pin.c_str(), e.local_path.c_str());
      return;
    case PinEventKind::kSkew:
      Log(EchoVR::LogLevel::Warning, "[NEVR.MAPS] map_skew reason=%s id=%s level=%s pin=%s fetch=%s notice=\"%s\"",
          e.reason.c_str(), e.id.c_str(), level.c_str(), pin.c_str(), e.fetch ? "true" : "false", e.notice.c_str());
      return;
    case PinEventKind::kLeaveScheduled:
      Log(EchoVR::LogLevel::Info, "[NEVR.MAPS] map_skew reason=leave_scheduled id=%s", e.id.c_str());
      return;
    case PinEventKind::kLeaveFailed:
      Log(EchoVR::LogLevel::Warning,
          "[NEVR.MAPS] map_skew reason=leave_failed id=%s retry_ms=%llu consequence=\"stays in a session whose map version "
          "differs\"",
          e.id.c_str(), static_cast<unsigned long long>(kLeaveRetryMs));
      return;
    case PinEventKind::kNoticeNotWritten:
      Log(EchoVR::LogLevel::Warning, "[NEVR.MAPS] map_skew reason=notice_not_written id=%s", e.id.c_str());
      return;
  }
}

class WinGameAccess final : public GameAccess {
 public:
  bool Callable() override {
    if (checked_) return callable_;
    checked_ = true;
    std::uint8_t* base = GameBase();
    if (base == nullptr) {
      LogPinOff(PinOffFields("no_game_module", nullptr, 0, nullptr, nullptr, 0));
      return false;
    }
    callable_ = ProveEntries(base);
    if (callable_) {
      set_ = reinterpret_cast<OwnerSetSymbolFn>(base + nevr_patch_addresses::CNSLOBBY_OWNER_SYMBOL_SET);
      get_ = reinterpret_cast<OwnerGetSymbolFn>(base + nevr_patch_addresses::CNSLOBBY_OWNER_SYMBOL_GET);
    }
    return callable_;
  }

  bool IsServer() override { return g_isServer != FALSE; }

  std::uint64_t Lobby() override {
    const void* netGame = nevr_return_to_lobby_queue::NetGameFromContext(g_pGame);
    if (netGame == nullptr) return 0;
    const void* clientLobby = nevr_return_to_lobby_queue::ReadPointer(netGame, nevr_patch_addresses::NETGAME_CLIENT_LOBBY_OFFSET);
    return clientLobby == nullptr
               ? 0
               : reinterpret_cast<std::uint64_t>(clientLobby) + nevr_patch_addresses::CLIENT_LOBBY_LOBBY_OFFSET;
  }

  std::int32_t NetState() override {
    const void* netGame = nevr_return_to_lobby_queue::NetGameFromContext(g_pGame);
    if (netGame == nullptr) return 0;
    std::int32_t state = 0;
    std::memcpy(&state, static_cast<const std::uint8_t*>(netGame) + nevr_patch_addresses::NETGAME_STATE_OFFSET, sizeof(state));
    return state;
  }

  std::uint64_t OwnerSymbol(std::uint64_t lobby, const char* key) override {
    std::uint64_t value = kOwnerSymbolAbsent;
    get_(lobby, &value, key, kOwnerSymbolAbsent);
    return value;
  }

  void SetOwnerSymbol(std::uint64_t lobby, const char* key, std::uint64_t value) override { set_(lobby, key, value); }

  std::uint64_t MainLevel() override { return EchoVR::ReadMainLevelSymbol(g_pGame, EchoVR::kPcMainLevelSymbolOffset); }

  // The notice goes into the net game's error block (lobby_failure_message_rules.h), never into the image.
  bool WriteNotice(const char* title, const char* line) override {
    return nevr_lobby_failure_message::WriteErrorBlock(nevr_map_session_pin_notice::NoticeBaseFor(g_pGame), title, line,
                                                       std::strlen(line) + 1);
  }

  bool ScheduleLeave() override {
    return nevr_return_to_lobby::ScheduleNow(nevr_return_to_lobby_queue::NetGameFromContext(g_pGame), "map_pin");
  }

  std::uint64_t NowMs() override { return GetTickCount64(); }

  void Event(const PinEvent& event) override { WriteEvent(event); }

 private:
  bool checked_ = false;
  bool callable_ = false;
  OwnerSetSymbolFn set_ = nullptr;
  OwnerGetSymbolFn get_ = nullptr;
};

WinGameAccess& Game() {
  static WinGameAccess game;
  return game;
}

}  // namespace

std::uint64_t SessionLevel() { return SessionLevelWith(Game()); }

void Tick() { TickWith(Game()); }

}  // namespace nevr_map_session_pin
