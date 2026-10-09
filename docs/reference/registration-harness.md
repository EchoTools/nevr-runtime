# Server registration against a local fake ServerDB

`src/runtime/tests/test_registration_roundtrip.cpp` (in the `test_protobuf_transport` executable, run by
`just test-auth-unit`) exercises server registration without a live service and without the game.

## What it runs

| Step | Runtime code under test |
| --- | --- |
| Registration envelope | `GameServer::BuildRegistrationEnvelope` / `FormatRegistrationVersion` (`src/runtime/server/registration_envelope.cpp`); `GameServerLib::RequestRegistration` (`src/runtime/server/gameserver_serverdb.cpp`) and the reconnect handler (`src/runtime/server/gameserver_callbacks.cpp`) both build their envelope through it |
| Connect with the bearer token | `WebSocketClient::Connect` (`src/runtime/server/websocket_client.cpp`); the fake asserts `Authorization: Bearer <token>` on the upgrade |
| Send | `GameServer::SendProtobufEnvelope` (`src/runtime/server/protobuf_transport.cpp`); the fake asserts the frame magic, the protobuf symbol and the decoded `GameServerRegistrationMessage` fields |
| Receive | the fake answers with a `GameServerRegistrationSuccessMessage` frame; `WebSocketClient::ProcessReceivedMessages` and `ParseServerDbFrame` deliver it and the test decodes it |

The fake is an `ix::WebSocketServer` on a random loopback port. Nothing leaves the machine.

## What it does not cover

- The engine calling `IServerLib::RequestRegistration` after its login, and the engine's handling of
  `SNSLobbyRegistrationSuccess`/`Failure` (`OnTcpMsgProtobuf` and the rejection path in `gameserver_callbacks.cpp`).
  A run of the real `echovr.exe -server` needs the login and config conversations
  (`src/runtime/compat/ws_bridge.cpp` conn 0 and 1) answered by a service.
- The HTTP token mint (`AcquireServerDbToken`): refresh-token exchange and the password RPC.
- Nakama's own registration handling (`regions=` / guild checks, `gg.IsServerHost`).
- The matchmaker path. Its routing over the shared login session (conn 2) is covered by the `TestHook_N61_*`
  tests in `src/runtime/tests/test_behavioral.cpp`; a matchmaker request through a real client needs a
  display (the nested `Xephyr :101`) and the owner's go for a client run.
