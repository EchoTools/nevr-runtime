#pragma once

// #45: a dedicated server begins multiplayer itself once its login completes. With a renderer the
// game's main menu does it: a menu timer begins multiplayer about ten seconds after the login. A
// server runs headless, no menu runs, and nothing begins it: the server stays "logged in", with no
// "Beginning multiplayer", no game server and no registration. At that switch the state machine
// queues the game's own begin (EchoVR::BeginMultiplayerLobby) on the game's thread.
// Pure: the decision only, no game memory.

#include <cstdint>

namespace nevr_server_bringup {

// ENetGameState values (abi/echovr.h NetGameState); state_machine.cpp asserts they match.
constexpr uint32_t kLoggingIn = 2;
constexpr uint32_t kLoggedIn = 3;

/// Whether the switch `from` -> `to` is the moment a server begins multiplayer: its own login
/// completing. "Logged in" reached any other way is not: beginning there would start a second
/// bring-up.
inline bool BeginsMultiplayer(bool isServer, uint32_t from, uint32_t to) {
  return isServer && from == kLoggingIn && to == kLoggedIn;
}

}  // namespace nevr_server_bringup
