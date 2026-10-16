#pragma once

#include "core/pch.h"
#include "abi/echovr.h"

VOID NetGameSwitchStateHook(PVOID pGame, EchoVR::NetGameState state);

/// Checks, once at boot on a dedicated server, the prologues of the two game functions it calls to
/// begin multiplayer after its login. A mismatch exits the server with that cause (ServerFatal);
/// a match logs the outcome at Info.
void ValidateServerBringup();
