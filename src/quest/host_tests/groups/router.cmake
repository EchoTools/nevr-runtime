# test-quest-router: the shared EVR session router and the Quest loopback transport on the host.
#   session_router_test        the router state machine through fake transports (login ordering, remote close, limits, backpressure)
#   ws_wire_test               RFC 6455 handshake and frame decoder under partial reads and bad input
#   loopback_game_server_test  the real loopback server + router + a raw TCP "game" client
#   remote_ws_test             the remote transport policy (wss only, one attempt, no downgrade) and worker through a fake
#                              connector and the real router
# The loopback server and its test write and parse their JSON records with nlohmann::json.

nevr_ht_group(router)
nevr_ht_test(session_router_test GROUP router HOST PTHREAD
  SRC src/runtime/compat/session_router.cpp src/runtime/compat/evr_codec.cpp src/quest/tests/session_router_test.cpp)
# The router test again with the standard library's checked containers: an erase through an iterator the container invalidated
# aborts there, where an unchecked build only gives wrong answers.
nevr_ht_test(session_router_test_checked GROUP router HOST PTHREAD DEFS _GLIBCXX_DEBUG
  SRC src/runtime/compat/session_router.cpp src/runtime/compat/evr_codec.cpp src/quest/tests/session_router_test.cpp)
nevr_ht_test(ws_wire_test GROUP router HOST PTHREAD
  SRC src/quest/net/ws_wire.cpp src/quest/tests/ws_wire_test.cpp)
nevr_ht_test(loopback_game_server_test GROUP router HOST PTHREAD
  SRC src/quest/net/ws_wire.cpp src/quest/net/loopback_game_server.cpp
      src/runtime/compat/session_router.cpp src/runtime/compat/evr_codec.cpp
      src/quest/tests/loopback_game_server_test.cpp)
nevr_ht_test(remote_ws_test GROUP router HOST PTHREAD
  SRC src/quest/net/remote_ws.cpp src/runtime/compat/session_router.cpp src/runtime/compat/evr_codec.cpp
      src/quest/tests/remote_ws_test.cpp)
# The loopback server under ThreadSanitizer: it has the accept thread, per-connection threads and the concurrent-Start test, so a
# data race on listenFd_/port_/token_ or an unsynchronised Start/Stop would be reported. -O1 also exercises warnings -O0 hides.
# halt_on_error makes any race fail the test.
nevr_ht_test(loopback_game_server_test_tsan GROUP router HOST PTHREAD TSAN ENV "TSAN_OPTIONS=halt_on_error=1 exitcode=66"
  SRC src/quest/net/ws_wire.cpp src/quest/net/loopback_game_server.cpp
      src/runtime/compat/session_router.cpp src/runtime/compat/evr_codec.cpp
      src/quest/tests/loopback_game_server_test.cpp)
