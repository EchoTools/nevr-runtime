#pragma once

// A dedicated server begins multiplayer itself once its login completes. With a renderer the
// game's main menu does it: a menu timer begins multiplayer about ten seconds after the login. A
// server runs headless, no menu runs, and nothing begins it: the server stays "logged in", with no
// "Beginning multiplayer", no game server and no registration. At that switch the state machine
// queues the game's own begin (EchoVR::BeginMultiplayerLobby) on the game's thread.
// Pure: the decisions only, no game memory and no logging; the callers (boot.cpp,
// state_machine.cpp) own the reads, the logs and the exit.

#include <cstddef>
#include <cstdint>

#include "abi/echovr.h"
#include "nevr_common.h"  // N97: the one ValidatePrologue

namespace nevr_server_bringup {

/// Prologues (echovr.exe 631547) of the two game functions the server bring-up calls.
/// 0x140104e70, the call queue: push rbp; sub rsp,0x100.
constexpr uint8_t kQueueGameThreadCallPrologue[9] = {0x40, 0x55, 0x48, 0x81, 0xEC, 0x00, 0x01, 0x00, 0x00};
/// 0x1400fddd0, begin multiplayer: sub rsp,0x78; xor eax,eax; mov dword [rsp+0x20],...
constexpr uint8_t kBeginMultiplayerLobbyPrologue[8] = {0x48, 0x83, 0xEC, 0x78, 0x33, 0xC0, 0xC7, 0x44};

/// Whether the switch `from` -> `to` is the moment a server begins multiplayer: its own login
/// completing. "Logged in" reached any other way is not: beginning there would start a second
/// bring-up.
inline bool BeginsMultiplayer(bool isServer, EchoVR::NetGameState from, EchoVR::NetGameState to) {
  return isServer && from == EchoVR::NetGameState::LoggingIn && to == EchoVR::NetGameState::LoggedIn;
}

/// Checks the prologues of the two functions a server calls to begin multiplayer, once, at boot.
/// Returns true when both match. On the first mismatch, calls `fail(cause)` with a short text that
/// names the function, and returns false. A null site is a mismatch (ValidatePrologue is false for it).
template <class Fail>
bool ValidatePrologues(const void* queueGameThreadCall, const void* beginMultiplayerLobby, Fail&& fail) {
  if (!nevr::ValidatePrologue(queueGameThreadCall, kQueueGameThreadCallPrologue,
                              sizeof(kQueueGameThreadCallPrologue))) {
    fail("server bring-up: QueueGameThreadCall prologue mismatch");
    return false;
  }
  if (!nevr::ValidatePrologue(beginMultiplayerLobby, kBeginMultiplayerLobbyPrologue,
                              sizeof(kBeginMultiplayerLobbyPrologue))) {
    fail("server bring-up: BeginMultiplayerLobby prologue mismatch");
    return false;
  }
  return true;
}

/// The queued call, on the game's thread: begins multiplayer when the net game is logged in, and
/// otherwise fails the server (`fail(state)`) rather than leaving it logged-out-or-loading and idle.
template <class Begin, class Fail>
void RunQueuedBegin(EchoVR::NetGameState state, Begin&& begin, Fail&& fail) {
  if (state != EchoVR::NetGameState::LoggedIn) {
    fail(state);
    return;
  }
  begin();
}

}  // namespace nevr_server_bringup
