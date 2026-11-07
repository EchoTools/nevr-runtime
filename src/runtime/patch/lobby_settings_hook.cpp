#include "runtime/patch/lobby_settings_hook.h"

#include <winsock2.h>
#include <windows.h>

#include <array>
#include <cstddef>
#include <cstdint>
#include <cstdio>
#include <cstring>
#include <string>

#include "core/logging.h"
#include "nevr/pc/nevr_common.h"
#include "runtime/compat/server_features.h"
#include "runtime/hook/hook_guard.h"
#include "runtime/hook/patching.h"

namespace nevr_lobby_settings_hook {
namespace {

// CR15NetGame::LobbySettingsCB(netGame, msg) (echovr.exe 0x14017de60): MOV [RSP+8],RBX; PUSH RDI; SUB RSP,0x30.
constexpr std::uint64_t kCallbackVA = 0x14017DE60;
constexpr std::array<std::uint8_t, 10> kCallbackPrologue = {0x48, 0x89, 0x5C, 0x24, 0x08, 0x57, 0x48, 0x83, 0xEC, 0x30};
using CallbackFn = void (*)(void* netGame, void* msg);

// The message the callback reads (0x14017de7b: MOV R8,[RBX+0x20]; 0x14017de7f: MOV RDX,[RBX+0x18]): the text, then its
// size, which CJson_LoadFromBuffer (0x1405f0bd0) takes; the text has no terminator.
constexpr std::size_t kMsgTextOffset = 0x18;
constexpr std::size_t kMsgSizeOffset = 0x20;
// The relay refuses a larger document (lobby_settings_relay.h kLobbySettingsMaxJsonBytes); a client reads no more.
constexpr std::uint64_t kMaxTextBytes = 1024ULL * 1024ULL;

std::uintptr_t g_gameBase = 0;
CallbackFn g_originalCallback = nullptr;

const char* ShownOrHidden(bool on) { return on ? "shown" : "hidden"; }

void Read(void* msg) {
  const char* text = nullptr;
  std::uint64_t size = 0;
  std::memcpy(&text, static_cast<const std::uint8_t*>(msg) + kMsgTextOffset, sizeof(text));
  std::memcpy(&size, static_cast<const std::uint8_t*>(msg) + kMsgSizeOffset, sizeof(size));
  nevr_server_features::Statement statement;
  if (text == nullptr || size > kMaxTextBytes) {
    statement.kind = nevr_server_features::Statement::Kind::kMalformed;
  } else {
    statement = nevr_server_features::ReadDocument(text, static_cast<std::size_t>(size));
  }
  const nevr_server_features::Gate gate = nevr_server_features::GateOf(statement);
  const bool changed = nevr_server_features::Client().Apply(gate);
  Log(statement.kind == nevr_server_features::Statement::Kind::kMalformed ? EchoVR::LogLevel::Warning
                                                                          : EchoVR::LogLevel::Info,
      "[NEVR.PATCH] lobby settings received statement=%s features=%s dropped=%zu bytes=%llu changed=%s (EchoCombat tac "
      "mods %s, MEDIGUN %s on this server)",
      nevr_server_features::KindName(statement.kind), nevr_server_features::Joined(statement.features).c_str(),
      statement.dropped, static_cast<unsigned long long>(size), changed ? "yes" : "no", ShownOrHidden(gate.echoCombat),
      ShownOrHidden(gate.medigun));
}

void CallbackHook(void* netGame, void* msg) {
  if (msg != nullptr) Read(msg);
  g_originalCallback(netGame, msg);
}

std::string HexBytes(const void* address, std::size_t count) {
  if (address == nullptr) return "unreadable";
  std::string out;
  char byte[4];
  for (std::size_t i = 0; i < count; ++i) {
    std::snprintf(byte, sizeof(byte), "%02x", static_cast<const std::uint8_t*>(address)[i]);
    out += byte;
  }
  return out;
}

}  // namespace

void Install(std::uintptr_t gameBase) {
  g_gameBase = gameBase;
  void* target = nevr::ResolveVA_Checked(g_gameBase, kCallbackVA);
  if (!nevr::ValidatePrologue(target, kCallbackPrologue.data(), kCallbackPrologue.size())) {
    Log(EchoVR::LogLevel::Warning,
        "[NEVR.PATCH] hook skipped name=LobbySettingsCB va=0x%llX reason=prologue_mismatch expected=%s actual=%s (no "
        "server's features are read: the EchoCombat tac mods and MEDIGUN stay hidden)",
        static_cast<unsigned long long>(kCallbackVA), HexBytes(kCallbackPrologue.data(), kCallbackPrologue.size()).c_str(),
        HexBytes(target, kCallbackPrologue.size()).c_str());
    return;
  }
  g_originalCallback = reinterpret_cast<CallbackFn>(target);
  if (PatchDetour(&g_originalCallback, reinterpret_cast<PVOID>(&CallbackHook), "LobbySettingsCB")) {
    Log(EchoVR::LogLevel::Info,
        "[NEVR.PATCH] hooked name=LobbySettingsCB va=0x%llX (the server's features are read from its lobby settings)",
        static_cast<unsigned long long>(kCallbackVA));
  }
}

}  // namespace nevr_lobby_settings_hook
