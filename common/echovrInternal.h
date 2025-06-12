#pragma once

#ifndef ECHOVR_INTERNAL_H
#define ECHOVR_INTERNAL_H

#include "echovr.h"
#include "pch.h"

namespace EchoVR {
// Obtain a handle for the game
extern CHAR* g_GameBaseAddress;

/// Obtains a pool item/block/memory page from a given pool for the given index.
typedef BYTE* PoolFindItemFunc(PVOID pool, UINT64 index);
extern PoolFindItemFunc* PoolFindItem;

/// Registers a callback for a certain type of websocket message.
/// <returns>An identifier for the callback registration, to be used for unregistering.</returns>
typedef UINT16 TcpBroadcasterListenFunc(EchoVR::TcpBroadcaster* broadcaster, EchoVR::SymbolId messageId, INT64 unk1,
                                        INT64 unk2, INT64 unk3, VOID* delegateProxy, BOOL prepend);
extern TcpBroadcasterListenFunc* TcpBroadcasterListen;

extern UINT64 TcpBroadcasterUnlisten(EchoVR::TcpBroadcaster* broadcaster, UINT16 cbResult);

/// Sends a message to a game server broadcaster.
/// <returns>TODO: Unverified, probably success result or size.</returns>
typedef INT32 BroadcasterSendFunc(EchoVR::Broadcaster* broadcaster, EchoVR::SymbolId messageId,
                                  INT32 mbThreadPriority,  // note: most use 0
                                  VOID* item, UINT64 size, VOID* buffer, UINT64 bufferLen, EchoVR::Peer peer,
                                  UINT64 dest, FLOAT priority, EchoVR::SymbolId unk);
extern BroadcasterSendFunc* BroadcasterSend;

/// Receives/relays a local event on the broadcaster, triggering a listener.
/// <returns>TODO: Unverified, probably success result.</returns>
typedef UINT64 BroadcasterReceiveLocalEventFunc(EchoVR::Broadcaster* broadcaster, EchoVR::SymbolId messageId,
                                                const CHAR* msgName, VOID* msg, UINT64 msgSize);
extern BroadcasterReceiveLocalEventFunc* BroadcasterReceiveLocalEvent;

/// <returns>An identifier for the callback registration, to be used for unregistering.</returns>
typedef UINT16 BroadcasterListenFunc(EchoVR::Broadcaster* broadcaster, EchoVR::SymbolId messageId,
                                     BOOL isReliableMsgType, VOID* px, BOOL prepend);
extern BroadcasterListenFunc* BroadcasterListen;

/// Unregisters a callback for a certain type of game broadcast message, using the return value from its registration.
/// <returns>None</returns>
typedef UINT64 BroadcasterUnlistenFunc(EchoVR::Broadcaster* broadcaster, UINT16 cbResult);
extern BroadcasterUnlistenFunc* BroadcasterUnlisten;

/// Obtains a JSON string value(with a default fallback value if it could not be obtained).
/// <returns>The resulting string returned from the JSON get string operation.</returns>
typedef CHAR* JsonValueAsStringFunc(EchoVR::Json* root, CHAR* keyName, CHAR* defaultValue, BOOL reportFailure);
extern JsonValueAsStringFunc* JsonValueAsString;

/// Parses a URI string into a URI container structure.
/// <returns>The result of the URI parsing operation.</returns>
typedef HRESULT UriContainerParseFunc(EchoVR::UriContainer* uriContainer, CHAR* uri);
extern UriContainerParseFunc* UriContainerParse;

/// Builds the CLI argument options and help descriptions list.
/// <returns>TODO: Unverified, probably success result</returns>
typedef UINT64 BuildCmdLineSyntaxDefinitionsFunc(PVOID pGame, PVOID pArgSyntax);
extern BuildCmdLineSyntaxDefinitionsFunc* BuildCmdLineSyntaxDefinitions;

/// Adds an argument to the CLI argument syntax object.
/// <returns>None</returns>
typedef VOID AddArgSyntaxFunc(PVOID pArgSyntax, const CHAR* sArgName, UINT64 minOptions, UINT64 maxOptions,
                              BOOL validate);
extern AddArgSyntaxFunc* AddArgSyntax;

/// Adds an argument help string to the CLI argument syntax object.
/// <returns>None</returns>
typedef VOID AddArgHelpStringFunc(PVOID pArgSyntax, const CHAR* sArgName, const CHAR* sArgHelpDescription);
extern AddArgHelpStringFunc* AddArgHelpString;

/// Processes the provided command line options for the running process.
/// <returns>TODO: Unverified, probably success result</returns>
typedef UINT64 PreprocessCommandLineFunc(PVOID pGame);
extern PreprocessCommandLineFunc* PreprocessCommandLine;

/// Writes a log to the logger, if all conditions such as log level are met.
/// <returns>None</returns>
typedef VOID WriteLogFunc(EchoVR::LogLevel logLevel, UINT64 unk, const CHAR* format, va_list vl);
extern WriteLogFunc* WriteLog;

/// TODO: Seemingly parses an HTTP/HTTPS URI to be connected to.
/// <returns>TODO: Unknown</returns>
typedef UINT64 HttpConnectFunc(VOID* unk, CHAR* uri);
extern HttpConnectFunc* HttpConnect;
typedef UINT64 LoadLocalConfigFunc(PVOID pGame);

/// Loads the local config (located at ./_local/config.json) for the provided game instance.
typedef UINT64 LoadLocalConfigFunc(PVOID pGame);
extern LoadLocalConfigFunc* LoadLocalConfig;
typedef VOID NetGameSwitchStateFunc(PVOID pGame, NetGameState state);

/// Switches net game state to a given new state (loading level, logging in, logged in, lobby, etc).
typedef VOID NetGameSwitchStateFunc(PVOID pGame, NetGameState state);
extern NetGameSwitchStateFunc* NetGameSwitchState;
typedef VOID NetGameScheduleReturnToLobbyFunc(PVOID pGame);

/// Schedules a return to the lobby in the net game.
typedef VOID NetGameScheduleReturnToLobbyFunc(PVOID pGame);
extern NetGameScheduleReturnToLobbyFunc* NetGameScheduleReturnToLobby;

typedef FARPROC GetProcAddressFunc(HMODULE hModule, LPCSTR lpProcName);

/// The game's definition for GetProcAddress.
/// Reference: https://learn.microsoft.com/en-us/windows/win32/api/libloaderapi/nf-libloaderapi-getprocaddress
typedef FARPROC GetProcAddressFunc(HMODULE hModule, LPCSTR lpProcName);
extern GetProcAddressFunc* GetProcAddress;
typedef UINT64 SetWindowTextAFunc(HWND hWnd, LPCSTR lpString);

/// The game's definition for SetWindowTitle.
/// Reference: https://learn.microsoft.com/en-us/windows/win32/api/winuser/nf-winuser-setwindowtexta
typedef UINT64 SetWindowTextAFunc(HWND hWnd, LPCSTR lpString);
extern SetWindowTextAFunc* SetWindowTextA_;
}  // namespace EchoVR

#endif  // ECHOVR_INTERNAL_H