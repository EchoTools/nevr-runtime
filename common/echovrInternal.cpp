#pragma once

#include "echovrInternal.h"

#include "echovr.h"
#include "pch.h"

namespace EchoVR {
// Obtain a handle for the game
CHAR* g_GameBaseAddress = (CHAR*)GetModuleHandle(NULL);

/// Obtains a pool item/block/memory page from a given pool for the given index.
PoolFindItemFunc* PoolFindItem = (PoolFindItemFunc*)(g_GameBaseAddress + 0x2CA9E0);

/// Registers a callback for a certain type of websocket message.
/// <returns>An identifier for the callback registration, to be used for unregistering.</returns>
TcpBroadcasterListenFunc* TcpBroadcasterListen = (TcpBroadcasterListenFunc*)(g_GameBaseAddress + 0xF81100);

/// Unregisters a callback for a certain type of websocket message, using the return value from its registration.
/// <returns>None</returns>

UINT64 TcpBroadcasterUnlisten(EchoVR::TcpBroadcaster* broadcaster, UINT16 cbResult) {
  // Obtain the listeners pool from the broadcaster structure.
  BYTE* listeners = (BYTE*)broadcaster->data + 352;

  // Obtain our block index and offset within it.
  UINT64 blockCapacity = *(UINT64*)(listeners + 40);
  UINT64 blockIndex = cbResult / blockCapacity;
  UINT64 indexInBlock = cbResult % blockCapacity;

  // Obtain the item from the pool
  UINT64 itemPage = NULL;
  if (blockIndex) {
    itemPage = *(UINT64*)(PoolFindItem(listeners, (blockIndex - 1) >> 1) + 8 * (blockIndex & 1));
  } else {
    itemPage = *(UINT64*)(listeners + 8);
  }

  // Free the item from the page at its given offset.
  UINT64 itemData = itemPage + 16;
  UINT64 result = itemData + (UINT32)(-((INT32)itemData) & 7);
  UINT32* flags = (UINT32*)(result + (indexInBlock * 80) + 12);
  *flags |= 1u;  // mark the page as free
  return result;
}

/// Sends a message to a game server broadcaster.
/// <returns>TODO: Unverified, probably success result or size.</returns>
BroadcasterSendFunc* BroadcasterSend = (BroadcasterSendFunc*)(g_GameBaseAddress + 0xF89AF0);

/// Receives/relays a local event on the broadcaster, triggering a listener.
/// <returns>TODO: Unverified, probably success result.</returns>
BroadcasterReceiveLocalEventFunc* BroadcasterReceiveLocalEvent =
    (BroadcasterReceiveLocalEventFunc*)(g_GameBaseAddress + 0xF87AA0);

/// Registers a callback for a certain type of game broadcaster message.
/// <returns>An identifier for the callback registration, to be used for unregistering.</returns>
BroadcasterListenFunc* BroadcasterListen = (BroadcasterListenFunc*)(g_GameBaseAddress + 0xF80ED0);

/// Unregisters a callback for a certain type of game broadcast message, using the return value from its registration.
/// <returns>None</returns>
BroadcasterUnlistenFunc* BroadcasterUnlisten = (BroadcasterUnlistenFunc*)(g_GameBaseAddress + 0xF8DF20);

/// Obtains a JSON string value(with a default fallback value if it could not be obtained).
/// <returns>The resulting string returned from the JSON get string operation.</returns>
JsonValueAsStringFunc* JsonValueAsString = (JsonValueAsStringFunc*)(g_GameBaseAddress + 0x5FE290);

/// Parses a URI string into a URI container structure.
/// <returns>The result of the URI parsing operation.</returns>
UriContainerParseFunc* UriContainerParse = (UriContainerParseFunc*)(g_GameBaseAddress + 0x621EC0);

/// Builds the CLI argument options and help descriptions list.
/// <returns>TODO: Unverified, probably success result</returns>
BuildCmdLineSyntaxDefinitionsFunc* BuildCmdLineSyntaxDefinitions =
    (BuildCmdLineSyntaxDefinitionsFunc*)(g_GameBaseAddress + 0xFEA00);

/// Adds an argument to the CLI argument syntax object.
/// <returns>None</returns>
AddArgSyntaxFunc* AddArgSyntax = (AddArgSyntaxFunc*)(g_GameBaseAddress + 0xD31B0);

/// Adds an argument help string to the CLI argument syntax object.
/// <returns>None</returns>
AddArgHelpStringFunc* AddArgHelpString = (AddArgHelpStringFunc*)(g_GameBaseAddress + 0xD30D0);

/// Processes the provided command line options for the running process.
/// <returns>TODO: Unverified, probably success result</returns>
PreprocessCommandLineFunc* PreprocessCommandLine = (PreprocessCommandLineFunc*)(g_GameBaseAddress + 0x116720);

/// Writes a log to the logger, if all conditions such as log level are met.
/// <returns>None</returns>
WriteLogFunc* WriteLog = (WriteLogFunc*)(g_GameBaseAddress + 0xEBE70);

/// TODO: Seemingly parses an HTTP/HTTPS URI to be connected to.
/// <returns>TODO: Unknown</returns>
HttpConnectFunc* HttpConnect = (HttpConnectFunc*)(g_GameBaseAddress + 0x1F60C0);

/// Loads the local config (located at ./_local/config.json) for the provided game instance.
LoadLocalConfigFunc* LoadLocalConfig = (LoadLocalConfigFunc*)(g_GameBaseAddress + 0x179EB0);

/// Switches net game state to a given new state (loading level, logging in, logged in, lobby, etc).
NetGameSwitchStateFunc* NetGameSwitchState = (NetGameSwitchStateFunc*)(g_GameBaseAddress + 0x1B8650);

/// Schedules a return to the lobby in the net game.
NetGameScheduleReturnToLobbyFunc* NetGameScheduleReturnToLobby =
    (NetGameScheduleReturnToLobbyFunc*)(g_GameBaseAddress + 0x1A89F0);

/// The game's definition for GetProcAddress.
/// Reference: https://learn.microsoft.com/en-us/windows/win32/api/libloaderapi/nf-libloaderapi-getprocaddress
GetProcAddressFunc* GetProcAddress = (GetProcAddressFunc*)(g_GameBaseAddress + 0xEAEF0);

/// The game's definition for SetWindowTitle.
/// Reference: https://learn.microsoft.com/en-us/windows/win32/api/winuser/nf-winuser-setwindowtexta
SetWindowTextAFunc* SetWindowTextA_ = (SetWindowTextAFunc*)(g_GameBaseAddress + 0x5105F0);
}  // namespace EchoVR
