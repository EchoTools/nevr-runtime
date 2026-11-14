// The session pin's PC half: echovr.exe's owner-JSON setter and getter (addresses.h), the lobby and the net game, the error block
// and the return to lobby. Each function's bytes are checked before it is called; a build that differs leaves the pin off and says
// so once. Every line the pin writes is formatted here (the core only decides and emits PinEvent records).

#include <winsock2.h>  // before windows.h (tools/tests/test_header_include_order.py)
#include <windows.h>

#include <cstddef>
#include <cstdint>
#include <cstdio>
#include <cstring>

#include "abi/game_level.h"
#include "core/logging.h"
#include "core/map_session_pin.h"
#include "runtime/content/map_session_pin_game.h"
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

// CR15NetClientLobby *(netgame+40h); its CNSLobby at +18h (veilnet.cpp NetLobby, the lobby its :1672 reads).
constexpr std::uintptr_t kNetGameClientLobbyOffset = 0x40;
constexpr std::uintptr_t kClientLobbyLobbyOffset = 0x18;
// "0x" and sixteen hex digits and the terminating NUL.
constexpr std::size_t kHexTextBytes = 24;

std::uint8_t* GameBase() { return reinterpret_cast<std::uint8_t*>(GetModuleHandleW(nullptr)); }

// The stock entry bytes, or another hook's jmp there whose tail is still stock (veilnet.cpp EntryOk).
bool EntryOk(const std::uint8_t* entry, const std::uint8_t* head, std::size_t n) {
  if (std::memcmp(entry, head, n) == 0) return true;
  return entry[0] == 0xe9 && n > 8 && std::memcmp(entry + 5, head + 5, n - 5) == 0;
}

std::string Hex(std::uint64_t value) {
  char text[kHexTextBytes];
  std::snprintf(text, sizeof(text), "0x%016llx", static_cast<unsigned long long>(value));
  return text;
}

// The log line for one record the core emitted. The text is the one the PC's log has always carried (docs/standards/logging.md
// [NEVR.MAPS]); a notice is quoted, and only ids, levels, pins and tokens are written, never a file's contents.
void WriteEvent(const PinEvent& e) {
  switch (e.kind) {
    case PinEventKind::kServerSet:
      Log(EchoVR::LogLevel::Info, "[NEVR.MAPS] session pin set level=%s pin=%s", Hex(e.level).c_str(), Hex(e.pin).c_str());
      return;
    case PinEventKind::kServerCleared:
      Log(EchoVR::LogLevel::Info, "[NEVR.MAPS] session pin cleared level=%s", Hex(e.level).c_str());
      return;
    case PinEventKind::kUnpinned:
      Log(EchoVR::LogLevel::Info, "[NEVR.MAPS] map_unpinned id=%s level=%s: the server states no map version", e.id.c_str(),
          Hex(e.level).c_str());
      return;
    case PinEventKind::kLocalMap:
      Log(EchoVR::LogLevel::Warning,
          "[NEVR.MAPS] map_skew reason=local_map id=%s: local %s overrides channel map %s; the server runs another "
          "version (pin %s). This match may not match the server. Remove the file to use the channel copy.",
          e.id.c_str(), e.local_path.c_str(), e.id.c_str(), Hex(e.pin).c_str());
      return;
    case PinEventKind::kSkew:
      Log(EchoVR::LogLevel::Warning, "[NEVR.MAPS] map_skew reason=%s id=%s level=%s pin=%s fetch=%d notice=\"%s\"",
          e.reason.c_str(), e.id.c_str(), Hex(e.level).c_str(), Hex(e.pin).c_str(), e.fetch ? 1 : 0,
          e.notice.c_str());
      return;
    case PinEventKind::kLeaveScheduled:
      Log(EchoVR::LogLevel::Info, "[NEVR.MAPS] map_skew leave scheduled id=%s", e.id.c_str());
      return;
    case PinEventKind::kLeaveFailed:
      Log(EchoVR::LogLevel::Warning,
          "[NEVR.MAPS] map_skew reason=leave_failed id=%s: the return to lobby could not be scheduled for %llu ms; this "
          "client stays in a session whose map version differs",
          e.id.c_str(), static_cast<unsigned long long>(kLeaveRetryMs));
      return;
    case PinEventKind::kNoticeNotWritten:
      Log(EchoVR::LogLevel::Warning, "[NEVR.MAPS] map_skew notice not written id=%s", e.id.c_str());
      return;
  }
}

class WinGameAccess final : public GameAccess {
 public:
  bool Callable() override {
    if (checked_) return callable_;
    checked_ = true;
    std::uint8_t* base = GameBase();
    const char* bad = base == nullptr ? "game module"
                      : !EntryOk(base + nevr_patch_addresses::CNSLOBBY_OWNER_SYMBOL_SET,
                                 nevr_patch_addresses::CNSLOBBY_OWNER_SYMBOL_SET_PROLOGUE,
                                 sizeof(nevr_patch_addresses::CNSLOBBY_OWNER_SYMBOL_SET_PROLOGUE))
                          ? "owner setter"
                      : !EntryOk(base + nevr_patch_addresses::CNSLOBBY_OWNER_SYMBOL_GET,
                                 nevr_patch_addresses::CNSLOBBY_OWNER_SYMBOL_GET_PROLOGUE,
                                 sizeof(nevr_patch_addresses::CNSLOBBY_OWNER_SYMBOL_GET_PROLOGUE))
                          ? "owner symbol getter"
                          : nullptr;
    callable_ = bad == nullptr;
    if (bad != nullptr)
      Log(EchoVR::LogLevel::Warning, "[NEVR.MAPS] session pin off: %s not callable (unexpected build)", bad);
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
    const void* clientLobby = nevr_return_to_lobby_queue::ReadPointer(netGame, kNetGameClientLobbyOffset);
    return clientLobby == nullptr ? 0 : reinterpret_cast<std::uint64_t>(clientLobby) + kClientLobbyLobbyOffset;
  }

  std::int32_t NetState() override {
    const void* netGame = nevr_return_to_lobby_queue::NetGameFromContext(g_pGame);
    if (netGame == nullptr) return 0;
    std::int32_t state = 0;
    std::memcpy(&state, netGame, sizeof(state));
    return state;
  }

  std::uint64_t OwnerSymbol(std::uint64_t lobby, const char* key) override {
    std::uint64_t value = 0;
    get_(lobby, &value, key, 0);
    return value;
  }

  void SetOwnerSymbol(std::uint64_t lobby, const char* key, std::uint64_t value) override { set_(lobby, key, value); }

  std::uint64_t MainLevel() override { return EchoVR::ReadMainLevelSymbol(g_pGame, EchoVR::kPcMainLevelSymbolOffset); }

  bool WriteNotice(const char* title, const char* line) override {
    return nevr_lobby_failure_message::WriteErrorBlock(GameBase(), title, line, std::strlen(line) + 1);
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
