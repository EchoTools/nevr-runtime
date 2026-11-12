// The session pin's game half: the owner-JSON symbol getter and setter, the lobby, the error block and the return to
// lobby. Calls to game code only; no byte of the game is written except the error block WriteErrorBlock owns.

#include <winsock2.h>  // before windows.h (tools/tests/test_header_include_order.py)
#include <windows.h>

#include <cstdint>
#include <cstring>

#include "abi/game_level.h"
#include "core/logging.h"
#include "runtime/content/map_session_pin.h"
#include "runtime/content/map_session_pin_seam.h"
#include "runtime/lifecycle/cli.h"
#include "runtime/lifecycle/config.h"
#include "runtime/lifecycle/return_to_lobby.h"
#include "runtime/lifecycle/return_to_lobby_queue.h"
#include "runtime/patch/lobby_failure_message_rules.h"

namespace nevr_map_session_pin {

namespace {

// CNSLobby's owner-JSON symbol setter and getter, as combat/veilnet.cpp names and checks them (its :671 and :715-717).
constexpr std::uintptr_t kOwnerSetSymbolRva = 0x615E60;
constexpr std::uintptr_t kOwnerGetSymbolRva = 0x60D3D0;
const std::uint8_t kOwnerSetSymbolHead[] = {0x48, 0x89, 0x5c, 0x24, 0x10, 0x48, 0x89, 0x6c, 0x24, 0x18, 0x48,
                                            0x89, 0x74, 0x24, 0x20, 0x57, 0x48, 0x83, 0xec, 0x30, 0x48, 0x8b,
                                            0xe9, 0x49, 0x8b, 0xd8, 0x48, 0x81, 0xc1, 0x10, 0x03};
const std::uint8_t kOwnerGetSymbolHead[] = {0x40, 0x53, 0x48, 0x83, 0xec, 0x30, 0x48, 0x81, 0xc1, 0x10, 0x03, 0x00, 0x00};
using OwnerSetSymbolFn = void(__fastcall*)(std::uint64_t lobby, const char* key, std::uint64_t value);
using OwnerGetSymbolFn = std::uint64_t*(__fastcall*)(std::uint64_t lobby, std::uint64_t* out, const char* key,
                                                      std::uint64_t fallback);

// CR15NetClientLobby *(netgame+40h); its CNSLobby at +18h (veilnet.cpp NetLobby, the lobby its :1672 reads).
constexpr std::uintptr_t kNetGameClientLobbyOffset = 0x40;
constexpr std::uintptr_t kClientLobbyLobbyOffset = 0x18;

std::uint8_t* GameBase() { return reinterpret_cast<std::uint8_t*>(GetModuleHandleW(nullptr)); }

// The stock entry bytes, or another hook's jmp there whose tail is still stock (veilnet.cpp EntryOk).
bool EntryOk(const std::uint8_t* entry, const std::uint8_t* head, std::size_t n) {
  if (std::memcmp(entry, head, n) == 0) return true;
  return entry[0] == 0xe9 && n > 8 && std::memcmp(entry + 5, head + 5, n - 5) == 0;
}

class WinGameAccess final : public GameAccess {
 public:
  bool Callable() override {
    if (checked_) return callable_;
    checked_ = true;
    std::uint8_t* base = GameBase();
    const char* bad = base == nullptr ? "game module"
                      : !EntryOk(base + kOwnerSetSymbolRva, kOwnerSetSymbolHead, sizeof(kOwnerSetSymbolHead))
                          ? "owner setter"
                      : !EntryOk(base + kOwnerGetSymbolRva, kOwnerGetSymbolHead, sizeof(kOwnerGetSymbolHead))
                          ? "owner symbol getter"
                          : nullptr;
    callable_ = bad == nullptr;
    if (bad != nullptr)
      Log(EchoVR::LogLevel::Warning, "[NEVR.MAPS] session pin off: %s not callable (unexpected build)", bad);
    if (callable_) {
      set_ = reinterpret_cast<OwnerSetSymbolFn>(base + kOwnerSetSymbolRva);
      get_ = reinterpret_cast<OwnerGetSymbolFn>(base + kOwnerGetSymbolRva);
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

std::uint64_t SessionLevel() { return SessionLevelOf(Game()); }

void Tick() { TickWith(Game()); }

}  // namespace nevr_map_session_pin
